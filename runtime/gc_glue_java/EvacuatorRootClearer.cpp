/*******************************************************************************
 * Copyright (c) 2015, 2017 IBM Corp. and others
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
#include "j2sever.h"
#include "ModronAssertions.h"

#if defined(OMR_GC_MODRON_SCAVENGER)
#include "ConfigurationDelegate.hpp"
#include "Dispatcher.hpp"
#include "Evacuator.hpp"
#include "FinalizableReferenceBuffer.hpp"
#include "FinalizableObjectBuffer.hpp"
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
#include "EvacuatorRootClearer.hpp"

void
MM_EvacuatorRootClearer::doSlot(omrobjectptr_t *slotPtr)
{
	getEvacuator()->evacuateRootObject(slotPtr, true);
}

#if defined(J9VM_GC_FINALIZATION)
void
MM_EvacuatorRootClearer::scanUnfinalizedObjects(MM_EnvironmentBase *env)
{
	Debug_MM_true(env == _env);
	/* allow the scavenger to handle this */
	if (_scavenger->isEvacuatorFlagSet(MM_EvacuatorDelegate::shouldScavengeUnfinalizedObjects)) {
		reportScanningStarted(RootScannerEntity_UnfinalizedObjects);
		scavengeUnfinalizedObjects();
		reportScanningEnded(RootScannerEntity_UnfinalizedObjects);
	}
}

MM_RootScanner::CompletePhaseCode
MM_EvacuatorRootClearer::scanUnfinalizedObjectsComplete(MM_EnvironmentBase *env)
{
	Debug_MM_true(env == _env);
	MM_RootScanner::CompletePhaseCode result = complete_phase_OK;
	/* ensure that all unfinalized processing is complete before we start marking additional objects */
	if (_scavenger->isEvacuatorFlagSet(MM_EvacuatorDelegate::shouldScavengeUnfinalizedObjects)) {
		reportScanningStarted(RootScannerEntity_UnfinalizedObjectsComplete);
		_env->_currentTask->synchronizeGCThreads(_env, UNIQUE_ID);
		if (!getEvacuator()->evacuateHeap()) {
			result = complete_phase_ABORT;
		}
		reportScanningEnded(RootScannerEntity_UnfinalizedObjectsComplete);
	}
	return result;
}
#endif /* J9VM_GC_FINALIZATION */

void
MM_EvacuatorRootClearer::scanSoftReferenceObjects(MM_EnvironmentBase *env)
{
	Debug_MM_true(env == _env);
	if (_scavenger->isEvacuatorFlagSet(MM_EvacuatorDelegate::shouldScavengeSoftReferenceObjects)) {
		reportScanningStarted(RootScannerEntity_SoftReferenceObjects);
		scavengeReferenceObjects(J9AccClassReferenceSoft);
		reportScanningEnded(RootScannerEntity_SoftReferenceObjects);
	}
}

MM_RootScanner::CompletePhaseCode
MM_EvacuatorRootClearer::scanSoftReferencesComplete(MM_EnvironmentBase *env)
{
	Debug_MM_true(env == _env);
	/* do nothing -- no new objects could have been discovered by soft reference processing */
	return complete_phase_OK;
}

void
MM_EvacuatorRootClearer::scanWeakReferenceObjects(MM_EnvironmentBase *env)
{
	Debug_MM_true(env == _env);
	if (_scavenger->isEvacuatorFlagSet(MM_EvacuatorDelegate::shouldScavengeWeakReferenceObjects)) {
		reportScanningStarted(RootScannerEntity_WeakReferenceObjects);
		scavengeReferenceObjects(J9AccClassReferenceWeak);
		reportScanningEnded(RootScannerEntity_WeakReferenceObjects);
	}
}

MM_RootScanner::CompletePhaseCode
MM_EvacuatorRootClearer::scanWeakReferencesComplete(MM_EnvironmentBase *env)
{
	/* No new objects could have been discovered by soft / weak reference processing,
	 * but we must complete this phase prior to unfinalized processing to ensure that
	 * finalizable referents get cleared */
	if (_scavenger->isAnyEvacuatorFlagSet(MM_EvacuatorDelegate::shouldScavengeWeakReferenceObjects | MM_EvacuatorDelegate::shouldScavengeSoftReferenceObjects)) {
		_env->_currentTask->synchronizeGCThreads(_env, UNIQUE_ID);
	}
	return complete_phase_OK;
}

void
MM_EvacuatorRootClearer::scanPhantomReferenceObjects(MM_EnvironmentBase *env)
{
	Debug_MM_true(env == _env);
	if (_scavenger->isEvacuatorFlagSet(MM_EvacuatorDelegate::shouldScavengePhantomReferenceObjects)) {
		reportScanningStarted(RootScannerEntity_PhantomReferenceObjects);
		scavengeReferenceObjects(J9AccClassReferencePhantom);
		reportScanningEnded(RootScannerEntity_PhantomReferenceObjects);
	}
}

MM_RootScanner::CompletePhaseCode
MM_EvacuatorRootClearer::scanPhantomReferencesComplete(MM_EnvironmentBase *env)
{
	Debug_MM_true(env == _env);
	CompletePhaseCode result = complete_phase_OK;
	if (_scavenger->isEvacuatorFlagSet(MM_EvacuatorDelegate::shouldScavengePhantomReferenceObjects)) {
		reportScanningStarted(RootScannerEntity_PhantomReferenceObjectsComplete);
		if (_env->_currentTask->synchronizeGCThreadsAndReleaseSingleThread(_env, UNIQUE_ID)) {
			_env->_cycleState->_referenceObjectOptions |= MM_CycleState::references_clear_phantom;
			_env->_currentTask->releaseSynchronizedGCThreads(_env);
		}

		/* phantom reference processing may resurrect objects - scan them now */
		if (!getEvacuator()->evacuateHeap()) {
			result = complete_phase_ABORT;
		}

		reportScanningEnded(RootScannerEntity_PhantomReferenceObjectsComplete);
	}
	return result;
}

void
MM_EvacuatorRootClearer::doMonitorReference(J9ObjectMonitor *objectMonitor, GC_HashTableIterator *monitorReferenceIterator)
{
	J9ThreadAbstractMonitor * monitor = (J9ThreadAbstractMonitor*)objectMonitor->monitor;
	omrobjectptr_t objectPtr = (omrobjectptr_t )monitor->userData;
	if(getEvacuator()->isInEvacuate(objectPtr)) {
		MM_ForwardedHeader forwardedHeader(objectPtr, _env->compressObjectReferences());
		omrobjectptr_t forwardPtr = forwardedHeader.getForwardedObject();
		if(NULL != forwardPtr) {
			monitor->userData = (uintptr_t)forwardPtr;
		} else {
			monitorReferenceIterator->removeSlot();
			/* We must call objectMonitorDestroy (as opposed to omrthread_monitor_destroy) when the
			 * monitor is not internal to the GC
			 */
			static_cast<J9JavaVM*>(_omrVM->_language_vm)->internalVMFunctions->objectMonitorDestroy(static_cast<J9JavaVM*>(_omrVM->_language_vm), (J9VMThread *)_env->getLanguageVMThread(), (omrthread_monitor_t)monitor);
		}
	}
}

void
MM_EvacuatorRootClearer::scanJNIWeakGlobalReferences(MM_EnvironmentBase *env)
{
	Debug_MM_true(env == _env);
#if defined(OMR_GC_CONCURRENT_SCAVENGER)
	/*
	 * Currently Concurrent Scavenger replaces STW Scavenger, so this check is not necessary
	 * (Concurrent Scavenger is always in progress)
	 * However Concurrent Scavenger runs might be interlaced with STW Scavenger time to time
	 * (for example for reducing amount of floating garbage)
	 */
	if (!_scavenger->isConcurrentCycleInProgress())
#endif /* defined(OMR_GC_CONCURRENT_SCAVENGER) */
	{
		MM_RootScanner::scanJNIWeakGlobalReferences(_env);
	}
}

void
MM_EvacuatorRootClearer::doJNIWeakGlobalReference(omrobjectptr_t *slotPtr)
{
	omrobjectptr_t objectPtr = *slotPtr;
	if(objectPtr && getEvacuator()->isInEvacuate(objectPtr)) {
		MM_ForwardedHeader forwardedHeader(objectPtr, _env->compressObjectReferences());
		*slotPtr = forwardedHeader.getForwardedObject();
	}
}

#if defined(J9VM_OPT_JVMTI)
void
MM_EvacuatorRootClearer::doJVMTIObjectTagSlot(omrobjectptr_t *slotPtr, GC_JVMTIObjectTagTableIterator *objectTagTableIterator)
{
	omrobjectptr_t objectPtr = *slotPtr;
	if(objectPtr && getEvacuator()->isInEvacuate(objectPtr)) {
		MM_ForwardedHeader forwardedHeader(objectPtr, _env->compressObjectReferences());
		*slotPtr = forwardedHeader.getForwardedObject();
	}
}
#endif /* J9VM_OPT_JVMTI */

void
MM_EvacuatorRootClearer::processReferenceList(MM_HeapRegionDescriptorStandard* region, omrobjectptr_t headOfList, MM_ReferenceStats *referenceStats)
{
	/* no list can possibly contain more reference objects than there are bytes in a region. */
	const uintptr_t maxObjects = region->getSize();
	uintptr_t objectsVisited = 0;
	GC_FinalizableReferenceBuffer buffer(_extensions);

	MM_Evacuator *evacuator = getEvacuator();
	omrobjectptr_t referenceObj = headOfList;
	while (NULL != referenceObj) {
		objectsVisited += 1;
		referenceStats->_candidates += 1;

		Assert_MM_true(objectsVisited < maxObjects);
		Assert_GC_true_with_message(_env, _scavenger->isObjectInNewSpace(referenceObj), "Scavenged reference object not in new space: %p\n", referenceObj);

		omrobjectptr_t nextReferenceObj = _extensions->accessBarrier->getReferenceLink(referenceObj);
		GC_SlotObject referentSlotObject(_extensions->getOmrVM(), J9GC_J9VMJAVALANGREFERENCE_REFERENT_ADDRESS(_env, referenceObj));
		omrobjectptr_t referent = referentSlotObject.readReferenceFromSlot();
		if (NULL != referent) {
			/* update the referent if it's been forwarded */
			MM_ForwardedHeader forwardedReferent(referent, _env->compressObjectReferences());
			if (forwardedReferent.isForwardedPointer()) {
				referent = forwardedReferent.getForwardedObject();
				referentSlotObject.writeReferenceToSlot(referent);
			}

			if (getEvacuator()->isInEvacuate(referent)) {
				uintptr_t referenceObjectType = J9CLASS_FLAGS(J9GC_J9OBJECT_CLAZZ(referenceObj, _env)) & J9AccClassReferenceMask;
				/* transition the state to cleared */
				Assert_MM_true(GC_ObjectModel::REF_STATE_INITIAL == J9GC_J9VMJAVALANGREFERENCE_STATE(_env, referenceObj));
				J9GC_J9VMJAVALANGREFERENCE_STATE(_env, referenceObj) = GC_ObjectModel::REF_STATE_CLEARED;

				referenceStats->_cleared += 1;

				/* Phantom references keep it's referent alive in Java 8 and doesn't in Java 9 and later */
				J9JavaVM * javaVM = (J9JavaVM*)_env->getLanguageVM();
				if ((J9AccClassReferencePhantom == referenceObjectType) && ((J2SE_VERSION(javaVM) & J2SE_VERSION_MASK) <= J2SE_18)) {
					/* Scanning will be done after the enqueuing */
					evacuator->evacuateRootObject(&referentSlotObject, true);
				} else {
					referentSlotObject.writeReferenceToSlot(NULL);
				}

				/* Check if the reference has a queue */
				if (0 != J9GC_J9VMJAVALANGREFERENCE_QUEUE(_env, referenceObj)) {
					/* Reference object can be enqueued onto the finalizable list */
					buffer.add(_env, referenceObj);
					referenceStats->_enqueued += 1;
					_scavenger->setEvacuatorFlag(MM_EvacuatorDelegate::finalizationRequired, true);
				}
			}
		}

		referenceObj = nextReferenceObj;
	}
	buffer.flush(_env);
}

void
MM_EvacuatorRootClearer::scavengeReferenceObjects(uintptr_t referenceObjectType)
{
	Assert_MM_true(getEvacuator()->getEnvironment()->getGCEnvironment()->_referenceObjectBuffer->isEmpty());

	MM_ScavengerJavaStats *javaStats = &getEvacuator()->getEnvironment()->getGCEnvironment()->_scavengerJavaStats;
	MM_HeapRegionDescriptorStandard *region = NULL;
	GC_HeapRegionIteratorStandard regionIterator(_extensions->heapRegionManager);
	while(NULL != (region = regionIterator.nextRegion())) {
		if (MEMORY_TYPE_NEW == (region->getTypeFlags() & MEMORY_TYPE_NEW)) {
			MM_HeapRegionDescriptorStandardExtension *regionExtension = MM_ConfigurationDelegate::getHeapRegionDescriptorStandardExtension(_env, region);
			for (uintptr_t i = 0; i < regionExtension->_maxListIndex; i++) {
				/* NOTE: we can't look at the list to determine if there's work to do since another thread may have already processed it and deleted everything */
				if(J9MODRON_HANDLE_NEXT_WORK_UNIT(_env)) {
					MM_ReferenceObjectList *list = &regionExtension->_referenceObjectLists[i];
					MM_ReferenceStats *stats = NULL;
					j9object_t head = NULL;
					switch (referenceObjectType) {
						case J9AccClassReferenceWeak:
						list->startWeakReferenceProcessing();
						if (!list->wasWeakListEmpty()) {
							head = list->getPriorWeakList();
							stats = &javaStats->_weakReferenceStats;
						}
						break;
						case J9AccClassReferenceSoft:
						list->startSoftReferenceProcessing();
						if (!list->wasSoftListEmpty()) {
							head = list->getPriorSoftList();
							stats = &javaStats->_softReferenceStats;
						}
						break;
						case J9AccClassReferencePhantom:
						list->startPhantomReferenceProcessing();
						if (!list->wasPhantomListEmpty()) {
							head = list->getPriorPhantomList();
							stats = &javaStats->_phantomReferenceStats;
						}
						break;
						default:
						Assert_MM_unreachable();
						break;
					}
					if (NULL != head) {
						processReferenceList(region, head, stats);
					}
				}
			}
		}
	}
	Assert_MM_true(getEvacuator()->getEnvironment()->getGCEnvironment()->_referenceObjectBuffer->isEmpty());
}

#if defined(J9VM_GC_FINALIZATION)
void
MM_EvacuatorRootClearer::scavengeUnfinalizedObjects()
{
	GC_FinalizableObjectBuffer buffer(_extensions);
	MM_HeapRegionDescriptorStandard *region = NULL;
	GC_HeapRegionIteratorStandard regionIterator(_extensions->heapRegionManager);
	MM_Evacuator *evacuator = getEvacuator();
	GC_Environment *gcEnv = evacuator->getEnvironment()->getGCEnvironment();
	while(NULL != (region = regionIterator.nextRegion())) {
		if (MEMORY_TYPE_NEW == (region->getTypeFlags() & MEMORY_TYPE_NEW)) {
			MM_HeapRegionDescriptorStandardExtension *regionExtension = MM_ConfigurationDelegate::getHeapRegionDescriptorStandardExtension(_env, region);
			for (uintptr_t i = 0; i < regionExtension->_maxListIndex; i++) {
				MM_UnfinalizedObjectList *list = &regionExtension->_unfinalizedObjectLists[i];
				if (!list->wasEmpty()) {
					if(J9MODRON_HANDLE_NEXT_WORK_UNIT(_env)) {
						omrobjectptr_t object = list->getPriorList();
						while (NULL != object) {
							omrobjectptr_t next = NULL;
							gcEnv->_scavengerJavaStats._unfinalizedCandidates += 1;

							MM_ForwardedHeader forwardedHeader(object, _env->compressObjectReferences());
							if (!forwardedHeader.isForwardedPointer()) {
								Assert_MM_true(evacuator->isInEvacuate(object));
								next = _extensions->accessBarrier->getFinalizeLink(object);
								omrobjectptr_t finalizableObject = evacuator->evacuateRootObject(&forwardedHeader, true);
								if (evacuator->isInEvacuate(finalizableObject)) {
									/* Failure - the scavenger must back out the work it has done. */
									gcEnv->_unfinalizedObjectBuffer->add(_env, object);
								} else {
									/* object was not previously forwarded -- it is now finalizable so push it to the local buffer */
									buffer.add(_env, finalizableObject);
									gcEnv->_scavengerJavaStats._unfinalizedEnqueued += 1;
									_scavenger->setEvacuatorFlag(MM_EvacuatorDelegate::finalizationRequired, true);
								}
							} else {
								omrobjectptr_t forwardedPtr =  forwardedHeader.getForwardedObject();
								Assert_MM_true(NULL != forwardedPtr);
								next = _extensions->accessBarrier->getFinalizeLink(forwardedPtr);
								gcEnv->_unfinalizedObjectBuffer->add(_env, forwardedPtr);
							}

							object = next;
						}
					}
				}
			}
		}
	}
	/* Flush the local buffer of finalizable objects to the global list */
	buffer.flush(_env);

	/* restore everything to a flushed state before exiting */
	gcEnv->_unfinalizedObjectBuffer->flush(_env);
}
#endif /* J9VM_GC_FINALIZATION */
#endif /* defined(OMR_GC_MODRON_SCAVENGER) */



