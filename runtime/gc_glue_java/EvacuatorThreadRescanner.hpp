
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

#ifndef EVACUATORTHREADRESCANNER_HPP_
#define EVACUATORTHREADRESCANNER_HPP_

#include "j9cfg.h"
#include "ModronAssertions.h"

#if defined(OMR_GC_MODRON_SCAVENGER)
#include "EnvironmentStandard.hpp"
#include "RootScanner.hpp"

/* Evacuator.hpp can't be included here as it would create a circular dependency */

class MM_Evacuator;

/**
 * A specialized root scanner for rescanning thread slots in order to auto-remember stack- (or thread-)
 * referenced object
 */
class MM_EvacuatorThreadRescanner : public MM_RootScanner
{
private:

public:
	MM_EvacuatorThreadRescanner(MM_EnvironmentBase *env) :
	MM_RootScanner(env)
	{
		_typeId = __FUNCTION__;
		setNurseryReferencesOnly(true);
	};

	virtual void doStackSlot(omrobjectptr_t *slotPtr, void *walkState, const void* stackLocation);

	virtual void doVMThreadSlot(omrobjectptr_t *slotPtr, GC_VMThreadIterator *vmThreadIterator);

	virtual void doSlot(omrobjectptr_t *slotPtr) {
		/* we only process thread and stack slots with this scanner */
		Assert_MM_unreachable();
	}

	virtual void doClass(J9Class *clazz) {
		/* we do not process classes in this scanner */
		Assert_MM_unreachable();
	}

#if defined(J9VM_GC_FINALIZATION)
	virtual void doFinalizableObject(omrobjectptr_t object) {
		Assert_MM_unreachable();
	}
#endif /* J9VM_GC_FINALIZATION */
};
#endif /* defined(OMR_GC_MODRON_SCAVENGER) */

#endif /* EVACUATORTHREADRESCANNER_HPP_ */
