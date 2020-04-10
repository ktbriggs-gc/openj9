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
 * SPDX-License-Identifier: EPL-2.0 OR Apache-2.0
 *******************************************************************************/

#include "Evacuator.hpp"
#include "EvacuatorThreadRescanner.hpp"

#if defined(OMR_GC_MODRON_SCAVENGER)
void
MM_EvacuatorThreadRescanner::doStackSlot(omrobjectptr_t *slotPtr, void *walkState, const void* stackLocation) {
	((MM_EnvironmentStandard *)_env)->getEvacuator()->rescanThreadSlot(slotPtr);
}

void
MM_EvacuatorThreadRescanner::doVMThreadSlot(omrobjectptr_t *slotPtr, GC_VMThreadIterator *vmThreadIterator) {
	((MM_EnvironmentStandard *)_env)->getEvacuator()->rescanThreadSlot(slotPtr);
}
#endif /* defined(OMR_GC_MODRON_SCAVENGER) */
