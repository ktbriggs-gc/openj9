/*******************************************************************************
 * Copyright (c) 2016, 2020 IBM Corp. and others
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

/**
 * @file
 * @ingroup GC_Structs
 */

#if !defined(LINKEDOBJECTSCANNER_HPP_)
#define LINKEDOBJECTSCANNER_HPP_

#include "MixedObjectScanner.hpp"

class GC_LinkedObjectScanner : public GC_MixedObjectScanner
{
	/* Data Members */
private:
	const uintptr_t _fieldOffset1;
	const uintptr_t _fieldOffset2;

protected:

public:

	/* Member Functions */
private:

protected:
	/**
	 * @param env The scanning thread environment
	 * @param objectPtr the object to be processed
	 * @param flags Scanning context flags
	 */
	MMINLINE GC_LinkedObjectScanner(MM_EnvironmentBase *env, omrobjectptr_t objectPtr, uintptr_t flags)
		: GC_MixedObjectScanner(env, objectPtr, (flags | linkedObjectScanner))
		, _fieldOffset1(J9GC_J9OBJECT_CLAZZ(objectPtr, env)->selfReferencingField1)
		, _fieldOffset2(J9GC_J9OBJECT_CLAZZ(objectPtr, env)->selfReferencingField2)
	{
		_typeId = __FUNCTION__;
	}

	/**
	 * Subclasses must call this method to set up the instance description bits and description pointer.
	 * @param[in] env The scanning thread environment
	 */
	MMINLINE void
	initialize(MM_EnvironmentBase *env, J9Class *clazzPtr)
	{
		GC_MixedObjectScanner::initialize(env, clazzPtr);
	}

public:
	/**
	 * In-place instantiation and initialization for mixed object scanner.
	 * @param[in] env The scanning thread environment
	 * @param[in] objectPtr The object to scan
	 * @param[in] allocSpace Pointer to space for in-place instantiation (at least sizeof(GC_MixedObjectScanner) bytes)
	 * @param[in] flags Scanning context flags
	 * @return Pointer to GC_MixedObjectScanner instance in allocSpace
	 */
	MMINLINE static GC_LinkedObjectScanner *
	newInstance(MM_EnvironmentBase *env, omrobjectptr_t objectPtr, void *allocSpace, uintptr_t flags)
	{
		GC_LinkedObjectScanner *objectScanner = (GC_LinkedObjectScanner *)allocSpace;

		new(objectScanner) GC_LinkedObjectScanner(env, objectPtr, flags);
		objectScanner->initialize(env, J9GC_J9OBJECT_CLAZZ(objectPtr, env));

		return objectScanner;
	}

	virtual uintptr_t
	getNextSelfReferencingSlotOffset(uintptr_t lastSelfReferencingFieldOffset)
	{
		if ((0 == lastSelfReferencingFieldOffset) && (0 < _fieldOffset1)) {
			return _fieldOffset1;
		} else if ((_fieldOffset1 == lastSelfReferencingFieldOffset) && (0 < _fieldOffset2)) {
			return _fieldOffset2;
		} else {
			return 0;
		}
	}
};

#endif /* LINKEDOBJECTSCANNER_HPP_ */
