/*******************************************************************************
 * Copyright (c) 2018, 2018 IBM Corp. and others
 *
 * This program and the accompanying materials are made available under
 * the terms of the Eclipse Public License 2.0 which accompanies this
 * distribution and is available at https://www.eclipse.org/legal/epl-2.0/
 * or the Apache License, Version 2.0 which accompanies this distribution and
 * is available at https://www.apache.org/licenses/LICENSE-2.0.
 *
 * This Source Code may also be made available under the following
 * Secondary Licenses when the conditions for such availability set
 * forth in the Eclipse Public License, v. 2.0 are satisfied: GNU
 * General Public License, version 2 with the GNU Classpath
 * Exception [1] and GNU General Public License, version 2 with the
 * OpenJDK Assembly Exception [2].
 *
 * [1] https://www.gnu.org/software/classpath/license.html
 * [2] http://openjdk.java.net/legal/assembly-exception.html
 *
 * SPDX-License-Identifier: EPL-2.0 OR Apache-2.0 OR GPL-2.0 WITH Classpath-exception-2.0 OR LicenseRef-GPL-2.0 WITH Assembly-exception
 *******************************************************************************/

#include "j9.h"
#include "j9cfg.h"
#include "j9consts.h"
#include "j9nonbuilder.h"
#include "modron.h"

#include "ConfigurationDelegate.hpp"
#include "CycleState.hpp"
#include "Evacuator.hpp"
#include "EvacuatorDelegate.hpp"
#include "EvacuatorRootScanner.hpp"
#include "EvacuatorThreadRescanner.hpp"
#if defined(J9VM_GC_FINALIZATION)
#include "FinalizeListManager.hpp"
#include "FinalizerSupport.hpp"
#endif /* defined(J9VM_GC_FINALIZATION) */
#include "ForwardedHeader.hpp"
#include "GCExtensions.hpp"
#include "HeapRegionDescriptorStandard.hpp"
#include "HeapRegionIteratorStandard.hpp"
#include "ObjectAccessBarrier.hpp"
#include "OwnableSynchronizerObjectBuffer.hpp"
#include "OwnableSynchronizerObjectList.hpp"
#include "PointerArrayObjectScanner.hpp"
#include "ReferenceObjectBuffer.hpp"
#include "ReferenceObjectScanner.hpp"
#include "Scavenger.hpp"
#include "ScavengerJavaStats.hpp"
#include "SlotObject.hpp"
#include "SublistFragment.hpp"

bool
MM_EvacuatorDelegate::initialize(MM_Evacuator *evacuator, MM_Forge *forge, MM_EvacuatorController *controller)
{
	_evacuator = evacuator;
	_controller = (MM_Scavenger *)controller;
	_forge = forge;
	_rootScanner = (MM_EvacuatorRootScanner *)_forge->allocate(sizeof(MM_EvacuatorRootScanner), OMR::GC::AllocationCategory::FIXED, OMR_GET_CALLSITE());
	return (NULL != _rootScanner);
}

void
MM_EvacuatorDelegate::tearDown()
{
	if (NULL != _rootScanner) {
		_forge->free(_rootScanner);
	}
}

uintptr_t
MM_EvacuatorDelegate::prepareForEvacuation(MM_EnvironmentBase *env)
{
	MM_GCExtensions *extensions = MM_GCExtensions::getExtensions(env);

	/* Sum the count of OwnableSynchronizerObject candidates before clearing java stats */
	UDATA ownableSynchronizerCandidates = extensions->scavengerJavaStats._ownableSynchronizerNurserySurvived + extensions->allocationStats._ownableSynchronizerObjectCount;

	/* Clear the global java-only gc statistics */
	extensions->scavengerJavaStats.clear();

	/* Set the total number of ownableSynchronizerObject candidates for gc verbose report */
	extensions->scavengerJavaStats._ownableSynchronizerCandidates = ownableSynchronizerCandidates;

	/* Set up the OwnableSynchronizerObject lists in each standard heap region */
	MM_HeapRegionDescriptorStandard *region = NULL;
	GC_HeapRegionIteratorStandard regionIterator(extensions->heapRegionManager);
	while (NULL != (region = regionIterator.nextRegion())) {
		MM_HeapRegionDescriptorStandardExtension *regionExtension = MM_ConfigurationDelegate::getHeapRegionDescriptorStandardExtension(env, region);
		for (uintptr_t i = 0; i < regionExtension->_maxListIndex; i++) {
			MM_OwnableSynchronizerObjectList *list = &regionExtension->_ownableSynchronizerObjectLists[i];
			if ((MEMORY_TYPE_NEW == (region->getTypeFlags() & MEMORY_TYPE_NEW))) {
				list->startOwnableSynchronizerProcessing();
			} else {
				list->backupList();
			}
		}
	}

#if defined(J9VM_GC_FINALIZATION)
	/* record whether finalizable processing is required in this cycle */
	return (((MM_GCExtensions *)extensions)->finalizeListManager->isFinalizableObjectProcessingRequired()) ? shouldScavengeFinalizableObjects : 0;
#else
	return 0;
#endif /* J9VM_GC_FINALIZATION */
}

void
MM_EvacuatorDelegate::cycleStart()
{
	_cycleCleared = false;

	/* bind calling thread to this delegate instance */
	_env = _evacuator->getEnvironment();
	_objectModel = &(_env->getExtensions()->objectModel);

	/* prepare thread environment for the cycle */
	Debug_MM_true(0 == ((J9VMThread *)_env->getLanguageVMThread())->gcRememberedSet.count);
	_env->getGCEnvironment()->_scavengerJavaStats.clear();

	/* instantiate the root scanner for this cycle */
	new(_rootScanner) MM_EvacuatorRootScanner(_env, _controller);
}

void
MM_EvacuatorDelegate::cycleEnd()
{
#if defined(J9VM_GC_FINALIZATION)
	/* Alert the finalizer (one time) if work needs to be done */
	if (_controller->setEvacuatorFlag(finalizationRequired, false)) {
		/* flag was set and is now reset for all other evacuators so this thread will kick off finalization */
		J9JavaVM * javaVM = (J9JavaVM*)_env->getLanguageVM();
		omrthread_monitor_enter(javaVM->finalizeMasterMonitor);
		javaVM->finalizeMasterFlags |= J9_FINALIZE_FLAGS_MASTER_WAKE_UP;
		omrthread_monitor_notify_all(javaVM->finalizeMasterMonitor);
		omrthread_monitor_exit(javaVM->finalizeMasterMonitor);
	}
#endif
	_objectModel = NULL;
	_env = NULL;
}

bool
MM_EvacuatorDelegate::objectHasIndirectObjectsInNursery(omrobjectptr_t objectptr)
{
	J9Class *classToScan = J9VM_J9CLASS_FROM_HEAPCLASS((J9VMThread*)_env->getLanguageVMThread(), objectptr);
	Debug_MM_true(NULL != classToScan);

	/* Check if Class Object should be remembered */
	omrobjectptr_t classObjectPtr = (omrobjectptr_t)classToScan->classObject;
	Assert_MM_false(_evacuator->isInEvacuate(classObjectPtr));
	if (_evacuator->isInSurvivor(classObjectPtr)) {
		 return true;
	}

	/* Iterate though Class Statics */
	do {
		omrobjectptr_t *slotPtr = NULL;
		GC_ClassStaticsIterator classStaticsIterator(_env, classToScan);
		while(NULL != (slotPtr = (omrobjectptr_t*)classStaticsIterator.nextSlot())) {
			omrobjectptr_t objectptr = *slotPtr;
			if (NULL != objectptr){
				if (_evacuator->isInSurvivor(objectptr) || _evacuator->isInSurvivor(objectptr)){
					return true;
				}
			}
		}

		GC_ConstantPoolObjectSlotIterator constantPoolObjectSlotIterator((J9JavaVM*)_env->getLanguageVM(), classToScan);
		while (NULL != (slotPtr = (omrobjectptr_t*)constantPoolObjectSlotIterator.nextSlot())) {
			omrobjectptr_t objectptr = *slotPtr;
			if (NULL != objectptr) {
				if (_evacuator->isInSurvivor(objectptr) || _evacuator->isInSurvivor(objectptr)) {
					return true;
				}
			}
		}

		classToScan = classToScan->replacedClass;
	} while (NULL != classToScan);

	return false;
}

bool
MM_EvacuatorDelegate::scanIndirectObjects(omrobjectptr_t objectptr)
{
	bool shouldBeRemembered = false;

	J9Class *classPtr = J9VM_J9CLASS_FROM_HEAPCLASS((J9VMThread*)_env->getLanguageVMThread(), objectptr);
	Debug_MM_true(NULL != classPtr);
	J9Class *classToScan = classPtr;
	do {
		volatile omrobjectptr_t *slotPtr = NULL;
		GC_ClassStaticsIterator classStaticsIterator(_env, classToScan);
		while((slotPtr = classStaticsIterator.nextSlot()) != NULL) {
			if (_evacuator->evacuateRootObject(slotPtr)) {
				shouldBeRemembered = true;
			}
		}

		GC_ConstantPoolObjectSlotIterator constantPoolObjectSlotIterator((J9JavaVM*)_env->getLanguageVM(), classToScan);
		while ((slotPtr = constantPoolObjectSlotIterator.nextSlot()) != NULL) {
			if (_evacuator->evacuateRootObject(slotPtr)) {
				shouldBeRemembered = true;
			}
		}

		slotPtr = (omrobjectptr_t *)&(classToScan->classObject);
		if (_evacuator->evacuateRootObject(slotPtr)) {
			shouldBeRemembered = true;
		}
		classToScan = classToScan->replacedClass;
	} while (NULL != classToScan);

	return shouldBeRemembered;
}

void
MM_EvacuatorDelegate::scanRoots()
{
	_rootScanner->scanRoots(_env);
}

void
MM_EvacuatorDelegate::scanClearable()
{
	/* flush ownable synchronizer object buffer after rebuild the ownableSynchronizerObjectList during main scan phase */
	_env->getGCEnvironment()->_ownableSynchronizerObjectBuffer->flush(_env);
	/* complete clearing stage */
	_rootScanner->scanClearable(_env);
	_cycleCleared = true;
}

void
MM_EvacuatorDelegate::rescanThreadSlots()
{
	MM_EvacuatorThreadRescanner threadRescanner(_env);
	threadRescanner.scanThreads(_env);
}

void
MM_EvacuatorDelegate::flushForWaitState()
{
	/* flush reference object buffer after processing references in the clearing stage */
	_env->getGCEnvironment()->_referenceObjectBuffer->flush(_env);
}

void
MM_EvacuatorDelegate::flushForEndCycle()
{
	/* flush ownable synchronizer object buffer after rebuild the ownableSynchronizerObjectList during main scan phase */
	_env->getGCEnvironment()->_ownableSynchronizerObjectBuffer->flush(_env);
}

GC_ObjectScanner *
MM_EvacuatorDelegate::getOwnableSynchronizerObjectScanner(omrobjectptr_t objectptr, void *objectScannerState, uintptr_t flags)
{
	if (!_env->getExtensions()->isConcurrentScavengerEnabled()) {
		if (GC_ObjectScanner::isHeapScan(flags)) {
			omrobjectptr_t link = MM_GCExtensions::getExtensions(_env)->accessBarrier->isObjectInOwnableSynchronizerList(objectptr);
			/* if isObjectInOwnableSynchronizerList() return NULL, it means the object isn't in OwanbleSynchronizerList,
			 * it could be the constructing object which would be added in the list after the construction finish later. ignore the object to avoid duplicated reference in the list. */
			if (NULL != link) {
				/* this method expects the caller (scanObject) never pass the same object twice, which could cause circular loop when walk through the list.
				 * the assertion partially could detect duplication case */
				Assert_MM_false(_evacuator->isInSurvivor(link));
				_env->getGCEnvironment()->_ownableSynchronizerObjectBuffer->add(_env, objectptr);
				_env->getGCEnvironment()->_scavengerJavaStats._ownableSynchronizerTotalSurvived += 1;
				if (_evacuator->isInSurvivor(objectptr)) {
					_env->getGCEnvironment()->_scavengerJavaStats._ownableSynchronizerNurserySurvived += 1;
				}
			}
		}
	}
	return GC_MixedObjectScanner::newInstance(_env, objectptr, objectScannerState, flags);
}

fomrobject_t *
MM_EvacuatorDelegate::getIndexableDataBounds(omrobjectptr_t indexableObject, uintptr_t *numberOfElements)
{
	GC_ArrayObjectModel *indexableObjectModel = &_env->getExtensions()->indexableObjectModel;
	*numberOfElements = indexableObjectModel->getSizeInElements((J9IndexableObject *)indexableObject);
	return (fomrobject_t *)indexableObjectModel->getDataPointerForContiguous((J9IndexableObject *)indexableObject);
}

GC_ObjectScanner *
MM_EvacuatorDelegate::getPointerArrayObjectScanner(omrobjectptr_t objectptr, void *objectScannerState, uintptr_t flags)
{
	uintptr_t splitAmount = 0;
	flags |= GC_ObjectScanner::indexableObjectNoSplit;
	return GC_PointerArrayObjectScanner::newInstance(_env, objectptr, objectScannerState, flags, splitAmount);
}

GC_IndexableObjectScanner *
MM_EvacuatorDelegate::getSplitPointerArrayObjectScanner(omrobjectptr_t objectptr, void *objectScannerState, uintptr_t splitIndex, uintptr_t splitAmount, uintptr_t flags)
{
	Debug_MM_true(0 < splitAmount);
	Debug_MM_true(0 == (flags & GC_ObjectScanner::indexableObjectNoSplit));
	return GC_PointerArrayObjectScanner::newInstance(_env, objectptr, objectScannerState, flags, splitAmount, splitIndex);
}

GC_ObjectScanner *
MM_EvacuatorDelegate::getReferenceObjectScanner(omrobjectptr_t objectptr, void *objectScannerState, uintptr_t flags)
{
	GC_ObjectScanner *objectScanner = NULL;

	if (GC_ObjectScanner::isHeapScan(flags)) {
		uint32_t referenceState = J9GC_J9VMJAVALANGREFERENCE_STATE(_env, objectptr);
		bool isReferenceCleared = (GC_ObjectModel::REF_STATE_CLEARED == referenceState) || (GC_ObjectModel::REF_STATE_ENQUEUED == referenceState);
		bool isObjectInNewSpace = _evacuator->isInSurvivor(objectptr);
		bool shouldScavengeReferenceObject = isObjectInNewSpace && !isReferenceCleared;
		bool referentMustBeMarked = isReferenceCleared || !isObjectInNewSpace;
		bool referentMustBeCleared = false;

		J9Class *clazzPtr = (J9Class *)J9GC_J9OBJECT_CLAZZ(objectptr, _env);
		UDATA referenceObjectOptions = _env->_cycleState->_referenceObjectOptions;
		UDATA referenceObjectType = J9CLASS_FLAGS(clazzPtr) & J9AccClassReferenceMask;
		switch (referenceObjectType) {
		case J9AccClassReferenceWeak:
			referentMustBeCleared = (0 != (referenceObjectOptions & MM_CycleState::references_clear_weak));
			if (!referentMustBeCleared && shouldScavengeReferenceObject) {
				_controller->setEvacuatorFlag(shouldScavengeWeakReferenceObjects, true);
			}
			break;
		case J9AccClassReferenceSoft:
			referentMustBeCleared = (0 != (referenceObjectOptions & MM_CycleState::references_clear_soft));
			referentMustBeMarked = referentMustBeMarked || ((0 == (referenceObjectOptions & MM_CycleState::references_soft_as_weak))
				&& ((UDATA)J9GC_J9VMJAVALANGSOFTREFERENCE_AGE(_env, objectptr) < MM_GCExtensions::getExtensions(_env)->getDynamicMaxSoftReferenceAge())
			);
			if (!referentMustBeCleared && shouldScavengeReferenceObject) {
				_controller->setEvacuatorFlag(shouldScavengeSoftReferenceObjects, true);
			}
			break;
		case J9AccClassReferencePhantom:
			referentMustBeCleared = (0 != (referenceObjectOptions & MM_CycleState::references_clear_phantom));
			if (!referentMustBeCleared && shouldScavengeReferenceObject) {
				_controller->setEvacuatorFlag(shouldScavengePhantomReferenceObjects, true);
			}
			break;
		default:
			Assert_MM_unreachable();
		}

		GC_SlotObject referentPtr(_env->getOmrVM(), J9GC_J9VMJAVALANGREFERENCE_REFERENT_ADDRESS(_env, objectptr));
#if defined(EVACUATOR_DEBUG)
		if (_controller->_debugger.isDebugFlagSet(EVACUATOR_DEBUG_DELEGATE_REFERENCES)) {
			debugDelegateReference(EVACUATOR_DEBUG_DELEGATE_REFERENCE_SCAN_HEAP, objectptr, referentPtr.readReferenceFromSlot(), referenceObjectType, referentMustBeCleared,
					isObjectInNewSpace, referentMustBeMarked, shouldScavengeReferenceObject);
		}
#endif /* defined(EVACUATOR_DEBUG) */
		if (referentMustBeCleared) {
#if defined(EVACUATOR_DEBUG)
			if (_controller->_debugger.isDebugFlagSet(EVACUATOR_DEBUG_DELEGATE_REFERENCES)) {
				debugDelegateReference(EVACUATOR_DEBUG_DELEGATE_REFERENCE_CLEAR, objectptr, referentPtr.readReferenceFromSlot(), referenceObjectType, referentMustBeCleared,
						isObjectInNewSpace, referentMustBeMarked, shouldScavengeReferenceObject);
			}
#endif /* defined(EVACUATOR_DEBUG) */
			/* Discovering this object at this stage in the GC indicates that it is being resurrected. Clear its referent slot. */
			referentPtr.writeReferenceToSlot(NULL);
			/* record that the reference has been cleared if it's not already in the cleared or enqueued state */
			if (!isReferenceCleared) {
				J9GC_J9VMJAVALANGREFERENCE_STATE(_env, objectptr) = GC_ObjectModel::REF_STATE_CLEARED;
			}
		} else if (shouldScavengeReferenceObject) {
#if defined(EVACUATOR_DEBUG)
			if (_controller->_debugger.isDebugFlagSet(EVACUATOR_DEBUG_DELEGATE_REFERENCES)) {
				debugDelegateReference(EVACUATOR_DEBUG_DELEGATE_REFERENCE_ADD, objectptr, referentPtr.readReferenceFromSlot(), referenceObjectType, referentMustBeCleared,
						isObjectInNewSpace, referentMustBeMarked, shouldScavengeReferenceObject);
			}
#endif /* defined(EVACUATOR_DEBUG) */
			_env->getGCEnvironment()->_referenceObjectBuffer->add(_env, objectptr);
		}

		fomrobject_t *referentSlotAddress = referentMustBeMarked ? NULL : referentPtr.readAddressFromSlot();
		objectScanner = GC_ReferenceObjectScanner::newInstance(_env, objectptr, referentSlotAddress, objectScannerState, flags);
	} else {
#if defined(EVACUATOR_DEBUG)
		if (_controller->_debugger.isDebugFlagSet(EVACUATOR_DEBUG_DELEGATE_REFERENCES)) {
			GC_SlotObject referentPtr(_env->getOmrVM(), J9GC_J9VMJAVALANGREFERENCE_REFERENT_ADDRESS(_env, objectptr));
			UDATA referenceObjectType = J9CLASS_FLAGS((J9Class *)J9GC_J9OBJECT_CLAZZ(objectptr, _env)) & J9AccClassReferenceMask;
			debugDelegateReference(EVACUATOR_DEBUG_DELEGATE_REFERENCE_SCAN_ROOT, objectptr, referentPtr.readReferenceFromSlot(), referenceObjectType, false,
					false, false, false);
		}
#endif /* defined(EVACUATOR_DEBUG) */
		objectScanner = GC_MixedObjectScanner::newInstance(_env, objectptr, objectScannerState, flags);
	}

	return objectScanner;
}

#if defined(EVACUATOR_DEBUG)
void
MM_EvacuatorDelegate::debugValidateObject(omrobjectptr_t objectptr)
{
	J9Class *clazz = (J9Class *)J9GC_J9OBJECT_CLAZZ(objectptr, _env);
	if ((uintptr_t)0x99669966 != clazz->eyecatcher) {
		OMRPORT_ACCESS_FROM_ENVIRONMENT(_env);
		omrtty_printf("%5lu %2llu %2llu:    assert; Invalid object header: object=%p; clazz=%p; *object=%p\n;", _controller->getEpoch()->gc, (uint64_t)_controller->getEpoch()->epoch, (uint64_t)_evacuator->getWorkerIndex(), objectptr, clazz, *objectptr);
		Debug_MM_true(false);
	}
}

void
MM_EvacuatorDelegate::debugValidateObject(MM_ForwardedHeader *forwardedHeader)
{
	J9Class* clazz = _objectModel->getPreservedClass(forwardedHeader);
	if ((uintptr_t)0x99669966 != clazz->eyecatcher) {
		OMRPORT_ACCESS_FROM_ENVIRONMENT(_env);
		omrtty_printf("%5lu %2llu %2llu:    assert; Invalid forwarded header: object=%p; clazz=%p; *object=%p; forwarded=%p\n;", _controller->getEpoch()->gc, (uint64_t)_controller->getEpoch()->epoch, (uint64_t)_evacuator->getWorkerIndex(), forwardedHeader->getObject(), clazz, *(forwardedHeader->getObject()), forwardedHeader->getForwardedObject());
		Debug_MM_true(false);
	}
}
#endif /* defined(EVACUATOR_DEBUG) */
