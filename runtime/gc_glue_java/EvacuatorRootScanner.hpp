
/*******************************************************************************
 * Copyright (c) 1991, 2018 IBM Corp. and others
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

#ifndef EVACUATORROOTSCANNER_HPP_
#define EVACUATORROOTSCANNER_HPP_

#include "j9cfg.h"
#include "j9consts.h"
#include "ModronAssertions.h"

#if defined(OMR_GC_MODRON_SCAVENGER)
#include "CollectorLanguageInterfaceImpl.hpp"
#include "EnvironmentStandard.hpp"
#include "EvacuatorRootClearer.hpp"
#include "FinalizeListManager.hpp"
#include "GCExtensions.hpp"
#include "OwnableSynchronizerObjectBuffer.hpp"
#include "ParallelTask.hpp"
#include "ReferenceObjectBuffer.hpp"
#include "RootScanner.hpp"
#include "EvacuatorRootClearer.hpp"
#include "EvacuatorThreadRescanner.hpp"
#include "StackSlotValidator.hpp"
#include "VMThreadIterator.hpp"

class MM_Scavenger;

/**
 * The root set scanner for MM_Scavenger.
 * @copydoc MM_RootScanner
 * @ingroup GC_Modron_Standard
 */
class MM_EvacuatorRootScanner : public MM_RootScanner
{
	/**
	 * Data members
	 */
private:
	MM_Scavenger *_scavenger;
	MM_EvacuatorRootClearer _rootClearer;

protected:

public:

	/**
	 * Function members
	 */
private:
	MM_Evacuator *getEvacuator() { return ((MM_EnvironmentStandard *)_env)->getEvacuator();	}

#if defined(J9VM_GC_FINALIZATION)
	void startUnfinalizedProcessing();
	void scavengeFinalizableObjects();
#endif /* defined(J9VM_GC_FINALIZATION) */

protected:

public:
	MM_EvacuatorRootScanner(MM_EnvironmentBase *env, MM_Scavenger *scavenger);

	/*
	 * Handle stack and thread slots specially so that we can auto-remember stack-referenced objects
	 */
	virtual void doStackSlot(omrobjectptr_t *slotPtr, void *walkState, const void* stackLocation);

	/*
	 * Handle stack and thread slots specially so that we can auto-remember stack-referenced objects
	 */
	virtual void doVMThreadSlot(omrobjectptr_t *slotPtr, GC_VMThreadIterator *vmThreadIterator);

	virtual void doSlot(omrobjectptr_t *slotPtr);

	virtual void
	doClass(J9Class *clazz)
	{
		/* we do not process classes in the scavenger */
		assume0(0);
	}

#if defined(J9VM_GC_FINALIZATION)
	virtual void
	scanRoots(MM_EnvironmentBase *env)
	{
		MM_RootScanner::scanRoots(env);
		startUnfinalizedProcessing();
	}

	virtual void
	doFinalizableObject(omrobjectptr_t object)
	{
		Assert_MM_unreachable();
	}

	virtual void scanFinalizableObjects(MM_EnvironmentBase *env);
#endif /* J9VM_GC_FINALIZATION */

	virtual void
	scanClearable(MM_EnvironmentBase *env)
	{
		Assert_MM_true(env == _env);
		if (env->_currentTask->synchronizeGCThreadsAndReleaseSingleThread(env, UNIQUE_ID)) {
			/* Soft and weak references resurrected by finalization need to be cleared immediately since weak and soft processing has already completed.
			 * This has to be set before unfinalizable (and phantom) processing
			 */
			env->_cycleState->_referenceObjectOptions |= MM_CycleState::references_clear_soft;
			env->_cycleState->_referenceObjectOptions |= MM_CycleState::references_clear_weak;
			env->_currentTask->releaseSynchronizedGCThreads(env);
		}
		Assert_GC_true_with_message(env, ((MM_EnvironmentStandard *)env)->getGCEnvironment()->_referenceObjectBuffer->isEmpty(), "Non-empty reference buffer in MM_EnvironmentBase* env=%p before scanClearable\n", env);
		_rootClearer.scanClearable(env);
		Assert_GC_true_with_message(env, _extensions->isScavengerBackOutFlagRaised() || ((MM_EnvironmentStandard *)env)->getGCEnvironment()->_referenceObjectBuffer->isEmpty(), "Non-empty reference buffer in MM_EnvironmentBase* env=%p after scanClearable\n", env);
	}

	virtual void scanJNIWeakGlobalReferences(MM_EnvironmentBase *env);

	void scavengeRememberedSet(MM_EnvironmentStandard *env);

	void
	rescanThreadSlots(MM_EnvironmentBase *env)
	{
		Assert_MM_true(env == _env);
		MM_EvacuatorThreadRescanner threadRescanner(env);
		threadRescanner.scanThreads(env);
	}

	void
	flush(MM_EnvironmentStandard *env)
	{
		Assert_MM_true((MM_EnvironmentBase *)env == _env);
		/* flush ownable synchronizer object buffer after rebuild the ownableSynchronizerObjectList during main scan phase */
		((MM_EnvironmentStandard *)env)->getGCEnvironment()->_ownableSynchronizerObjectBuffer->flush(env);
	}


};
#endif /* defined(OMR_GC_MODRON_SCAVENGER) */

#endif /* EVACUATORROOTSCANNER_HPP_ */
