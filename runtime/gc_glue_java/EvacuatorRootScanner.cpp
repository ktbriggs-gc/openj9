/*******************************************************************************
 * Copyright (c) 2015, 2018 IBM Corp. and others
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

#include "j9cfg.h"
#include "ModronAssertions.h"

#if defined(OMR_GC_MODRON_SCAVENGER)
#include "CollectorLanguageInterfaceImpl.hpp"
#include "ConfigurationDelegate.hpp"
#include "Dispatcher.hpp"
#include "Evacuator.hpp"
#include "FinalizableObjectBuffer.hpp"
#include "FinalizableReferenceBuffer.hpp"
#include "FinalizeListManager.hpp"
#include "ForwardedHeader.hpp"
#include "Heap.hpp"
#include "HeapRegionDescriptorStandard.hpp"
#include "HeapRegionIteratorStandard.hpp"
#include "ObjectAccessBarrier.hpp"
#include "ReferenceObjectBuffer.hpp"
#include "ReferenceObjectList.hpp"
#include "ReferenceStats.hpp"
#include "Scavenger.hpp"
#include "SlotObject.hpp"
#include "UnfinalizedObjectBuffer.hpp"
#include "UnfinalizedObjectList.hpp"

#include "EvacuatorRootScanner.hpp"

MM_EvacuatorRootScanner::MM_EvacuatorRootScanner(MM_EnvironmentBase *env, MM_Scavenger *scavenger)
	: MM_RootScanner(env)
	, _scavenger(scavenger)
	, _rootClearer(env, scavenger)
{
	_typeId = __FUNCTION__;
	setNurseryReferencesOnly(true);

	/*
	 * In the case of Concurrent Scavenger JNI Weak Global References required to be scanned as a hard root.
	 * The reason for this VM uses elements of table without calling a Read Barrier,
	 * so JNI Weak Global References table should be treated as a hard root until VM code is fixed
	 * and Read Barrier is called for each single object.
	 */
	_jniWeakGlobalReferencesTableAsRoot = _extensions->isConcurrentScavengerEnabled();
}

void
MM_EvacuatorRootScanner::doSlot(omrobjectptr_t *slotPtr)
{
	getEvacuator()->evacuateRootObject((volatile omrobjectptr_t *)slotPtr);
}

void
MM_EvacuatorRootScanner::doStackSlot(omrobjectptr_t *slotPtr, void *walkState, const void* stackLocation)
{
	if (_scavenger->isHeapObject(*slotPtr) && !_extensions->heap->objectIsInGap(*slotPtr)) {
		/* heap object - validate and mark */
		Assert_MM_validStackSlot(MM_StackSlotValidator(MM_StackSlotValidator::COULD_BE_FORWARDED, *slotPtr, stackLocation, walkState).validate(_env));
		getEvacuator()->evacuateThreadSlot((volatile omrobjectptr_t *)slotPtr);
	} else if (NULL != *slotPtr) {
		/* stack object - just validate */
		Assert_MM_validStackSlot(MM_StackSlotValidator(MM_StackSlotValidator::NOT_ON_HEAP, *slotPtr, stackLocation, walkState).validate(_env));
	}
}

void
MM_EvacuatorRootScanner::doVMThreadSlot(omrobjectptr_t *slotPtr, GC_VMThreadIterator *vmThreadIterator)
{
	if (_scavenger->isHeapObject(*slotPtr) && !_extensions->heap->objectIsInGap(*slotPtr)) {
		getEvacuator()->evacuateThreadSlot((volatile omrobjectptr_t *)slotPtr);
	} else if (NULL != *slotPtr) {
		Assert_GC_true_with_message4(_env, (vmthreaditerator_state_monitor_records == vmThreadIterator->getState()),
				"Thread %p structures scan: slot %p has bad value %p, iterator state %d\n", vmThreadIterator->getVMThread(), slotPtr, *slotPtr, vmThreadIterator->getState());
	}
}

void
MM_EvacuatorRootScanner::scanJNIWeakGlobalReferences(MM_EnvironmentBase *env)
{
	Debug_MM_true((MM_EnvironmentBase *)env == _env);

#if defined(OMR_GC_CONCURRENT_SCAVENGER)
	/*
	 * Currently Concurrent Scavenger replaces STW Scavenger, so this check is not necessary
	 * (Concurrent Scavenger is always in progress)
	 * However Concurrent Scavenger runs might be interlaced with STW Scavenger time to time
	 * (for example for reducing amount of floating garbage)
	 */
	if (_scavenger->isConcurrentCycleInProgress())
#endif /* defined(OMR_GC_CONCURRENT_SCAVENGER) */
	{
		MM_RootScanner::scanJNIWeakGlobalReferences(env);
	}
}

void
MM_EvacuatorRootScanner::scavengeRememberedSet(MM_EnvironmentStandard *env)
{
	Debug_MM_true((MM_EnvironmentBase *)env == _env);
	reportScanningStarted(RootScannerEntity_ScavengeRememberedSet);
	_scavenger->scavengeRememberedSet(env);
	reportScanningEnded(RootScannerEntity_ScavengeRememberedSet);
}

#if defined(J9VM_GC_FINALIZATION)
void
MM_EvacuatorRootScanner::scanFinalizableObjects(MM_EnvironmentBase *env)
{
	Debug_MM_true((MM_EnvironmentBase *)env == _env);
	reportScanningStarted(RootScannerEntity_FinalizableObjects);
	/* synchronization can be expensive so skip it if there's no work to do */
	if (_scavenger->isEvacuatorFlagSet(MM_EvacuatorDelegate::shouldScavengeFinalizableObjects)) {
		if (env->_currentTask->synchronizeGCThreadsAndReleaseSingleThread(env, UNIQUE_ID)) {
			scavengeFinalizableObjects();
			env->_currentTask->releaseSynchronizedGCThreads(env);
		}
	} else {
		/* double check that there really was no work to do */
		Assert_MM_true(!MM_GCExtensions::getExtensions(env)->finalizeListManager->isFinalizableObjectProcessingRequired());
	}
	reportScanningEnded(RootScannerEntity_FinalizableObjects);
}

void
MM_EvacuatorRootScanner::startUnfinalizedProcessing()
{
	if(J9MODRON_HANDLE_NEXT_WORK_UNIT(_env)) {
		_scavenger->setEvacuatorFlag(MM_EvacuatorDelegate::shouldScavengeUnfinalizedObjects, false);

		MM_HeapRegionDescriptorStandard *region = NULL;
		GC_HeapRegionIteratorStandard regionIterator(_env->getExtensions()->getHeap()->getHeapRegionManager());
		while(NULL != (region = regionIterator.nextRegion())) {
			if ((MEMORY_TYPE_NEW == (region->getTypeFlags() & MEMORY_TYPE_NEW))) {
				MM_HeapRegionDescriptorStandardExtension *regionExtension = MM_ConfigurationDelegate::getHeapRegionDescriptorStandardExtension(_env, region);
				for (UDATA i = 0; i < regionExtension->_maxListIndex; i++) {
					MM_UnfinalizedObjectList *list = &regionExtension->_unfinalizedObjectLists[i];
					list->startUnfinalizedProcessing();
					if (!list->wasEmpty()) {
						_scavenger->setEvacuatorFlag(MM_EvacuatorDelegate::shouldScavengeUnfinalizedObjects, true);
					}
				}
			}
		}
	}
}

void
MM_EvacuatorRootScanner::scavengeFinalizableObjects()
{
	GC_FinalizeListManager * const finalizeListManager = _extensions->finalizeListManager;
	MM_Evacuator *evacuator = getEvacuator();

	/* this code must be run single-threaded and we should only be here if work is actually required */
	Assert_MM_true(_env->_currentTask->isSynchronized());
	Assert_MM_true(_scavenger->isEvacuatorFlagSet(MM_EvacuatorDelegate::shouldScavengeFinalizableObjects));
	Assert_MM_true(finalizeListManager->isFinalizableObjectProcessingRequired());

	{
		GC_FinalizableObjectBuffer objectBuffer(_extensions);
		/* walk finalizable objects loaded by the system class loader */
		omrobjectptr_t systemObject = finalizeListManager->resetSystemFinalizableObjects();
		while (NULL != systemObject) {
			omrobjectptr_t next = NULL;
			if(_scavenger->isObjectInEvacuateMemory(systemObject)) {
				MM_ForwardedHeader forwardedHeader(systemObject, _env->compressObjectReferences());
				if (!forwardedHeader.isForwardedPointer()) {
					next = _extensions->accessBarrier->getFinalizeLink(systemObject);
					omrobjectptr_t copiedOrNot = evacuator->evacuateRootObject(&forwardedHeader, true);
					objectBuffer.add(_env, copiedOrNot);
				} else {
					omrobjectptr_t forwardedPtr =  forwardedHeader.getNonStrictForwardedObject();
					Assert_MM_true(NULL != forwardedPtr);
					next = _extensions->accessBarrier->getFinalizeLink(forwardedPtr);
					objectBuffer.add(_env, forwardedPtr);
				}
			} else {
				next = _extensions->accessBarrier->getFinalizeLink(systemObject);
				objectBuffer.add(_env, systemObject);
			}

			systemObject = next;
		}
		objectBuffer.flush(_env);
	}

	{
		GC_FinalizableObjectBuffer objectBuffer(_extensions);
		/* walk finalizable objects loaded by the all other class loaders */
		omrobjectptr_t defaultObject = finalizeListManager->resetDefaultFinalizableObjects();
		while (NULL != defaultObject) {
			omrobjectptr_t next = NULL;
			if(_scavenger->isObjectInEvacuateMemory(defaultObject)) {
				MM_ForwardedHeader forwardedHeader(defaultObject, _env->compressObjectReferences());
				if (!forwardedHeader.isForwardedPointer()) {
					next = _extensions->accessBarrier->getFinalizeLink(defaultObject);
					omrobjectptr_t copiedOrNot = evacuator->evacuateRootObject(&forwardedHeader, true);
					objectBuffer.add(_env, copiedOrNot);
				} else {
					omrobjectptr_t forwardedPtr = forwardedHeader.getNonStrictForwardedObject();
					Assert_MM_true(NULL != forwardedPtr);
					next = _extensions->accessBarrier->getFinalizeLink(forwardedPtr);
					objectBuffer.add(_env, forwardedPtr);
				}
			} else {
				next = _extensions->accessBarrier->getFinalizeLink(defaultObject);
				objectBuffer.add(_env, defaultObject);
			}

			defaultObject = next;
		}
		objectBuffer.flush(_env);
	}

	{
		/* walk reference objects */
		GC_FinalizableReferenceBuffer referenceBuffer(_extensions);
		omrobjectptr_t referenceObject = finalizeListManager->resetReferenceObjects();
		while (NULL != referenceObject) {
			omrobjectptr_t next = NULL;
			if(_scavenger->isObjectInEvacuateMemory(referenceObject)) {
				MM_ForwardedHeader forwardedHeader(referenceObject, _env->compressObjectReferences());
				if (!forwardedHeader.isForwardedPointer()) {
					next = _extensions->accessBarrier->getReferenceLink(referenceObject);
					omrobjectptr_t copiedOrNot = evacuator->evacuateRootObject(&forwardedHeader, true);
					referenceBuffer.add(_env, copiedOrNot);
				} else {
					omrobjectptr_t forwardedPtr =  forwardedHeader.getNonStrictForwardedObject();
					Assert_MM_true(NULL != forwardedPtr);
					next = _extensions->accessBarrier->getReferenceLink(forwardedPtr);
					referenceBuffer.add(_env, forwardedPtr);
				}
			} else {
				next = _extensions->accessBarrier->getReferenceLink(referenceObject);
				referenceBuffer.add(_env, referenceObject);
			}

			referenceObject = next;
		}
		referenceBuffer.flush(_env);
	}
}
#endif /* J9VM_GC_FINALIZATION */
#endif /* defined(OMR_GC_MODRON_SCAVENGER) */
