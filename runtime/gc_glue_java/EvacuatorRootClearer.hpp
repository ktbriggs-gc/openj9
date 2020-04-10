
/*******************************************************************************
 * Copyright (c) 1991, 2017 IBM Corp. and others
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

#ifndef EVACUATORROOTCLEARER_HPP_
#define EVACUATORROOTCLEARER_HPP_

#include "j9cfg.h"
#include "j9consts.h"
#include "ModronAssertions.h"

#if defined(OMR_GC_MODRON_SCAVENGER)
#include "EnvironmentStandard.hpp"
#include "ForwardedHeader.hpp"
#include "GCExtensions.hpp"
#include "ParallelTask.hpp"
#include "RootScanner.hpp"

class MM_HeapRegionDescriptorStandard;
class MM_Scavenger;

/**
 * The clearable root set scanner for MM_Scavenger.
 * @copydoc MM_RootScanner
 * @ingroup GC_Modron_Standard
 */
class MM_EvacuatorRootClearer : public MM_RootScanner
{
private:
	MM_Scavenger *_scavenger;

	void processReferenceList(MM_HeapRegionDescriptorStandard* region, omrobjectptr_t headOfList, MM_ReferenceStats *referenceStats);
	void scavengeReferenceObjects(uintptr_t referenceObjectType);
#if defined(J9VM_GC_FINALIZATION)
	void scavengeUnfinalizedObjects();
#endif /* defined(J9VM_GC_FINALIZATION) */

private:
	MM_Evacuator *getEvacuator() { return ((MM_EnvironmentStandard *)_env)->getEvacuator(); }
public:
	MM_EvacuatorRootClearer(MM_EnvironmentBase *env, MM_Scavenger *scavenger) :
	MM_RootScanner(env),
	_scavenger(scavenger)
	{
		_typeId = __FUNCTION__;
		setNurseryReferencesOnly(true);

		/*
		 * JNI Weak Global References table can be skipped in Clearable phase
		 * if it has been scanned as a hard root for Concurrent Scavenger already
		 */
		_jniWeakGlobalReferencesTableAsRoot = _extensions->isConcurrentScavengerEnabled();
	};

	virtual void doSlot(omrobjectptr_t *slotPtr);

	virtual void
	doClass(J9Class *clazz)
	{
		/* we do not process classes in the scavenger */
		assume0(0);
	}

	virtual void scanSoftReferenceObjects(MM_EnvironmentBase *env);

	virtual CompletePhaseCode scanSoftReferencesComplete(MM_EnvironmentBase *env);

	virtual void scanWeakReferenceObjects(MM_EnvironmentBase *env);

	virtual CompletePhaseCode scanWeakReferencesComplete(MM_EnvironmentBase *env);

#if defined(J9VM_GC_FINALIZATION)
	virtual void scanUnfinalizedObjects(MM_EnvironmentBase *env);

	virtual CompletePhaseCode scanUnfinalizedObjectsComplete(MM_EnvironmentBase *env);
#endif /* J9VM_GC_FINALIZATION */

	/* empty, move ownable synchronizer processing in main scan phase */
	virtual void scanOwnableSynchronizerObjects(MM_EnvironmentBase *env) {}

	virtual void scanPhantomReferenceObjects(MM_EnvironmentBase *env);

	virtual CompletePhaseCode scanPhantomReferencesComplete(MM_EnvironmentBase *env);

	virtual void doMonitorReference(J9ObjectMonitor *objectMonitor, GC_HashTableIterator *monitorReferenceIterator);

	virtual CompletePhaseCode
	scanMonitorReferencesComplete(MM_EnvironmentBase *env)
	{
		reportScanningStarted(RootScannerEntity_MonitorReferenceObjectsComplete);
		static_cast<J9JavaVM*>(_omrVM->_language_vm)->internalVMFunctions->objectMonitorDestroyComplete(static_cast<J9JavaVM*>(_omrVM->_language_vm), (J9VMThread *)env->getOmrVMThread()->_language_vmthread);
		reportScanningEnded(RootScannerEntity_MonitorReferenceObjectsComplete);
		return complete_phase_OK;
	}

	virtual void scanJNIWeakGlobalReferences(MM_EnvironmentBase *env);

	virtual void doJNIWeakGlobalReference(omrobjectptr_t *slotPtr);

#if defined(J9VM_OPT_JVMTI)
	virtual void doJVMTIObjectTagSlot(omrobjectptr_t *slotPtr, GC_JVMTIObjectTagTableIterator *objectTagTableIterator);
#endif /* J9VM_OPT_JVMTI */
#if defined(J9VM_GC_FINALIZATION)
	virtual void
	doFinalizableObject(omrobjectptr_t object)
	{
		Assert_MM_unreachable();
	}
#endif /* J9VM_GC_FINALIZATION */
};
#endif /* defined(OMR_GC_MODRON_SCAVENGER) */
#endif /* EVACUATORROOTCLEARER_HPP_ */
