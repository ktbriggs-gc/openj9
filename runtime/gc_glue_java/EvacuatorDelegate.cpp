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
#include "j9protos.h"
#include "modron.h"

#include "ConfigurationDelegate.hpp"
#include "CycleState.hpp"
#include "EnvironmentStandard.hpp"
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
	Debug_MM_true(env->isMasterThread());
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
MM_EvacuatorDelegate::mergeGCStats(MM_EnvironmentBase * envBase)
{
	MM_GCExtensions *ext = MM_GCExtensions::getExtensions(envBase);
	MM_ScavengerJavaStats *finalGCJavaStats = &ext->scavengerJavaStats;

	MM_EnvironmentStandard* env = MM_EnvironmentStandard::getEnvironment(envBase);
	MM_ScavengerJavaStats *scavJavaStats = &env->getGCEnvironment()->_scavengerJavaStats;

	finalGCJavaStats->_unfinalizedCandidates += scavJavaStats->_unfinalizedCandidates;
	finalGCJavaStats->_unfinalizedEnqueued += scavJavaStats->_unfinalizedEnqueued;

	finalGCJavaStats->_ownableSynchronizerCandidates += scavJavaStats->_ownableSynchronizerCandidates;
	finalGCJavaStats->_ownableSynchronizerTotalSurvived += scavJavaStats->_ownableSynchronizerTotalSurvived;
	finalGCJavaStats->_ownableSynchronizerNurserySurvived += scavJavaStats->_ownableSynchronizerNurserySurvived;

	finalGCJavaStats->_weakReferenceStats.merge(&scavJavaStats->_weakReferenceStats);
	finalGCJavaStats->_softReferenceStats.merge(&scavJavaStats->_softReferenceStats);
	finalGCJavaStats->_phantomReferenceStats.merge(&scavJavaStats->_phantomReferenceStats);

	scavJavaStats->clear();
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
	/* Alert the finalizer if work needs to be done */
	if (_env->isMasterThread() && _controller->isEvacuatorFlagSet(finalizationRequired)) {
		/* the master thread will kick off finalization */
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
	Debug_MM_true((uintptr_t)0x99669966 == classToScan->eyecatcher);
	do {
		/* Iterate over all slots in a class which contain an object reference */
		GC_ClassIterator classIterator(_env, classToScan);
		for (volatile j9object_t *slotPtr = classIterator.nextSlot(); NULL != slotPtr; slotPtr = classIterator.nextSlot()) {
			Assert_MM_false(_evacuator->isInEvacuate(*(omrobjectptr_t*)slotPtr));
			if (_evacuator->isInSurvivor(*(omrobjectptr_t*)slotPtr)) {
				return true;
			}
		}
		/* Check if Class Object should be remembered */
		Assert_MM_false(_evacuator->isInEvacuate((omrobjectptr_t)classToScan->classObject));
		if (_evacuator->isInSurvivor((omrobjectptr_t)classToScan->classObject)) {
			 return true;
		}
		classToScan = classToScan->replacedClass;
	} while (NULL != classToScan);

	return false;
}

bool
MM_EvacuatorDelegate::scanIndirectObjects(omrobjectptr_t objectptr)
{
	bool shouldBeRemembered = false;

	J9Class *classToScan = J9VM_J9CLASS_FROM_HEAPCLASS((J9VMThread*)_env->getLanguageVMThread(), objectptr);
	Debug_MM_true((uintptr_t)0x99669966 == classToScan->eyecatcher);
	do {
		GC_ClassIterator classIterator(_env, classToScan);
		/* Iterate over all slots in a class which contain an object reference and check if slot object should be evacuated */
		for (volatile j9object_t *slotPtr = classIterator.nextSlot(); NULL != slotPtr; slotPtr = classIterator.nextSlot()) {
			omrobjectptr_t forwardedAddress = _evacuator->evacuateRootObject((volatile omrobjectptr_t *)slotPtr);
			if (_evacuator->isInSurvivor(forwardedAddress) || _evacuator->isInEvacuate(forwardedAddress)) {
				shouldBeRemembered = true;
			}
		}
		/* Check if Class Object should be evacuated */
		omrobjectptr_t forwardedAddress = _evacuator->evacuateRootObject((volatile omrobjectptr_t *)&(classToScan->classObject));
		if (_evacuator->isInSurvivor(forwardedAddress) || _evacuator->isInEvacuate(forwardedAddress)) {
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
		if (_evacuator->isAnyDebugFlagSet(EVACUATOR_DEBUG_DELEGATE_REFERENCES)) {
			debugDelegateReference(referenceObjectOptions, EVACUATOR_DEBUG_DELEGATE_REFERENCE_SCAN_HEAP, objectptr, referentPtr.readReferenceFromSlot(), referenceObjectType, referentMustBeCleared,
					isObjectInNewSpace, referentMustBeMarked, shouldScavengeReferenceObject);
		}
#endif /* defined(EVACUATOR_DEBUG) */
		if (referentMustBeCleared) {
#if defined(EVACUATOR_DEBUG)
			if (_evacuator->isAnyDebugFlagSet(EVACUATOR_DEBUG_DELEGATE_REFERENCES)) {
				debugDelegateReference(referenceObjectOptions, EVACUATOR_DEBUG_DELEGATE_REFERENCE_CLEAR, objectptr, referentPtr.readReferenceFromSlot(), referenceObjectType, referentMustBeCleared,
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
			if (_evacuator->isAnyDebugFlagSet(EVACUATOR_DEBUG_DELEGATE_REFERENCES)) {
				debugDelegateReference(referenceObjectOptions, EVACUATOR_DEBUG_DELEGATE_REFERENCE_ADD, objectptr, referentPtr.readReferenceFromSlot(), referenceObjectType, referentMustBeCleared,
						isObjectInNewSpace, referentMustBeMarked, shouldScavengeReferenceObject);
			}
#endif /* defined(EVACUATOR_DEBUG) */
			_env->getGCEnvironment()->_referenceObjectBuffer->add(_env, objectptr);
		}

		fomrobject_t *referentSlotAddress = referentMustBeMarked ? NULL : referentPtr.readAddressFromSlot();
		objectScanner = GC_ReferenceObjectScanner::newInstance(_env, objectptr, referentSlotAddress, objectScannerState, flags);
	} else {
#if defined(EVACUATOR_DEBUG)
		if (_evacuator->isAnyDebugFlagSet(EVACUATOR_DEBUG_DELEGATE_REFERENCES)) {
			GC_SlotObject referentPtr(_env->getOmrVM(), J9GC_J9VMJAVALANGREFERENCE_REFERENT_ADDRESS(_env, objectptr));
			UDATA referenceObjectType = J9CLASS_FLAGS((J9Class *)J9GC_J9OBJECT_CLAZZ(objectptr, _env)) & J9AccClassReferenceMask;
			debugDelegateReference(_env->_cycleState->_referenceObjectOptions, EVACUATOR_DEBUG_DELEGATE_REFERENCE_SCAN_ROOT, objectptr, referentPtr.readReferenceFromSlot(), referenceObjectType, false,
					false, false, false);
		}
#endif /* defined(EVACUATOR_DEBUG) */
		objectScanner = GC_MixedObjectScanner::newInstance(_env, objectptr, objectScannerState, flags);
	}

	return objectScanner;
}

#if defined(EVACUATOR_DEBUG)
void
MM_EvacuatorDelegate::debugValidateObject(omrobjectptr_t objectptr, uintptr_t forwardedLength)
{
	J9Class *clazz = (J9Class *)J9GC_J9OBJECT_CLAZZ(objectptr, _env);
	if ((uintptr_t)0x99669966 != clazz->eyecatcher) {
		OMRPORT_ACCESS_FROM_ENVIRONMENT(_env);
		omrtty_printf("assert[%2llu]; Invalid object header: evacuator=%p; object=%p; clazz=%p; *object=%p\n",
				(uint64_t)_evacuator->getWorkerIndex(), _evacuator, objectptr, clazz, *objectptr);
		Debug_MM_true(false);
	} else if (forwardedLength != _objectModel->getConsumedSizeInBytesWithHeader(objectptr)) {
		OMRPORT_ACCESS_FROM_ENVIRONMENT(_env);
		omrtty_printf("assert[%2llu]; Invalid object size 0x%llx != 0x%llx class instance size: evacuator=%p; object=%p; clazz=%p; *object=%p\n",
				(uint64_t)_evacuator->getWorkerIndex(), forwardedLength, _objectModel->getConsumedSizeInBytesWithHeader(objectptr),
				_evacuator, objectptr, clazz, *objectptr);
		Debug_MM_true(false);
	}
}

void
MM_EvacuatorDelegate::debugValidateObject(MM_ForwardedHeader *forwardedHeader)
{
	J9Class* clazz = _objectModel->getPreservedClass(forwardedHeader);
	if ((uintptr_t)0x99669966 != clazz->eyecatcher) {
		OMRPORT_ACCESS_FROM_ENVIRONMENT(_env);
		omrtty_printf("assert[%2llu]; Invalid forwarded header: evacuator=%p; object=%p; clazz=%p; forwarded=%p\n",
				(uint64_t)_evacuator->getWorkerIndex(), _evacuator, forwardedHeader->getObject(), clazz, forwardedHeader->getForwardedObject());
		Debug_MM_true(false);
	}
}
#endif /* defined(EVACUATOR_DEBUG) */
