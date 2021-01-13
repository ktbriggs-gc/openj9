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

#if !defined(MIXEDOBJECTSCANNER_HPP_)
#define MIXEDOBJECTSCANNER_HPP_

#include "j9.h"
#include "j9cfg.h"
#include "modron.h"
#include "objectdescription.h"
#include "GCExtensions.hpp"
#include "ObjectModel.hpp"
#include "ObjectScanner.hpp"
#include "SlotObject.hpp"


/**
 * This class is used to iterate over the slots of a Java object.
 */
class GC_MixedObjectScanner : public GC_ObjectScanner
{

	/* Data Members */
private:

protected:
	uintptr_t *_descriptionPtr; /**< current description pointer */
#if defined(J9VM_GC_LEAF_BITS)
	uintptr_t *_leafPtr;
#endif /* J9VM_GC_LEAF_BITS */
	fomrobject_t *_endPtr;		/**< points to end of object */

public:

	/* Member Functions */
private:
protected:
	/**
	 * @param env The scanning thread environment
	 * @param objectPtr the object to be processed
	 * @param flags Scanning context flags
	 */
	MMINLINE GC_MixedObjectScanner(MM_EnvironmentBase *env, uintptr_t flags)
		: GC_ObjectScanner(env, NULL, 0, flags)
		, _descriptionPtr(NULL)
#if defined(J9VM_GC_LEAF_BITS)
		, _leafPtr(NULL)
#endif /* J9VM_GC_LEAF_BITS */
		, _endPtr(NULL)
	{
		_typeId = __FUNCTION__;
	}

	/**
	 * Subclasses must call this method to set up the instance description bits and description pointer.
	 * @param[in] env The scanning thread environment
	 */
	MMINLINE void
	initialize(MM_EnvironmentBase *env, omrobjectptr_t objectPtr)
	{
		/* Initialize the slot map from description bits */
		J9Class *classPtr = J9GC_J9OBJECT_CLAZZ(objectPtr, env);
		_descriptionPtr = classPtr->instanceDescription;
		_scanMap = (uintptr_t)_descriptionPtr;
#if defined(J9VM_GC_LEAF_BITS)
		_leafPtr = classPtr->instanceLeafDescription;
		_leafMap = (uintptr_t)_leafPtr;
#endif /* J9VM_GC_LEAF_BITS */
		if (_scanMap & 1) {
			_scanMap >>= 1;
			_descriptionPtr = NULL;
#if defined(J9VM_GC_LEAF_BITS)
			_leafMap >>= 1;
			_leafPtr = NULL;
#endif /* J9VM_GC_LEAF_BITS */
			setFlags(noMoreSlots, true);
		} else {
			_descriptionPtr = (uintptr_t *)_scanMap;
			_scanMap = *_descriptionPtr;
			_descriptionPtr += 1;
#if defined(J9VM_GC_LEAF_BITS)
			_leafPtr = (uintptr_t *)_leafMap;
			_leafMap = *_leafPtr;
			_leafPtr += 1;
#endif /* J9VM_GC_LEAF_BITS */
			setFlags(noMoreSlots, true);
		}
		
		/* cache the total consumed size in bytes of the object if it will fit in 32 bits -- this obviates a 2nd call to the object model when the object is scanned */
		GC_ObjectModel * const objectModel = &env->getExtensions()->objectModel;
		uintptr_t objectSize = objectModel->getConsumedSizeInBytesWithHeader(objectPtr);
		Debug_OS_true(objectModel->adjustSizeInBytes(objectSize) == objectSize);
		if (((uintptr_t)1 << 32) > objectSize) {
			objectSizeInBytes((uint32_t)objectSize);
		}

		/* set up the map base pointer and install it as the initial scan slot address flagged as unscanned */
		_endPtr = (fomrobject_t *)((uintptr_t)objectPtr + objectSize);
		fomrobject_t *scanPtr = (fomrobject_t *)((uintptr_t)objectPtr + objectModel->getHeaderSize(objectPtr));
		bool hasMoreSlots = (_bitsPerScanMap < GC_SlotObject::subtractSlotAddresses(_endPtr, scanPtr, compressObjectReferences()));
		setMapPtr(scanPtr, hasMoreSlots);
		GC_ObjectScanner::initialize(env);
		Debug_OS_true(hasMoreSlots == !isFlagSet(noMoreSlots));
	}

	/**
	 * Return base pointer and slot bit map for next block of contiguous slots to be scanned. The
	 * base pointer must be fomrobject_t-aligned. Bits in the bit map are scanned in order of
	 * increasing significance, and the least significant bit maps to the slot at the returned
	 * base pointer.
	 *
	 * @param[out] scanMap the bit map for the slots contiguous with the returned base pointer
	 * @param[out] hasNextSlotMap set this to true if this method should be called again, false if this map is known to be last
	 * @return a pointer to the first slot mapped by the least significant bit of the map, or NULL if no more slots
	 */
	virtual fomrobject_t *
#if defined(J9VM_GC_LEAF_BITS)
	getNextSlotMap(uintptr_t *slotMap, uintptr_t *leafMap, bool *hasNextSlotMap)
#else
	getNextSlotMap(uintptr_t *slotMap, bool *hasNextSlotMap)
#endif /* J9VM_GC_LEAF_BITS */
	{
		*slotMap = 0;
#if defined(J9VM_GC_LEAF_BITS)
		*leafMap = 0;
#endif /* J9VM_GC_LEAF_BITS */
		bool const compressed = compressObjectReferences();
		fomrobject_t *nextMapPtr = _mapPtr;
		while (_endPtr > nextMapPtr) {
			*slotMap = *_descriptionPtr;
			_descriptionPtr += 1;
#if defined(J9VM_GC_LEAF_BITS)
			*leafMap = *_leafPtr;
			_leafPtr += 1;
#endif /* J9VM_GC_LEAF_BITS */
			if (0 != *slotMap) {
				*hasNextSlotMap = _bitsPerScanMap < GC_SlotObject::subtractSlotAddresses(_endPtr, nextMapPtr, compressed);
				return nextMapPtr;
			}
			nextMapPtr = GC_SlotObject::addToSlotAddress(nextMapPtr, _bitsPerScanMap, compressed);
		}
		*hasNextSlotMap = false;
		return NULL;
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
	MMINLINE static GC_MixedObjectScanner *
	newInstance(MM_EnvironmentBase *env, omrobjectptr_t objectPtr, void *allocSpace, uintptr_t flags)
	{
		GC_MixedObjectScanner *objectScanner = (GC_MixedObjectScanner *)allocSpace;
		new(objectScanner) GC_MixedObjectScanner(env, flags);
		objectScanner->initialize(env, objectPtr);
		return objectScanner;
	}

	uintptr_t
	getBytesRemaining()
	{
		uintptr_t endPtr = (uintptr_t)_endPtr;
		uintptr_t scanPtr = (uintptr_t)getScanPtr();
		if (((uintptr_t)NULL != scanPtr) && (scanPtr <= endPtr)) {
			Debug_OS_true((uintptr_t)_mapPtr <= scanPtr);
			Debug_OS_true(scanPtr < ((uintptr_t)_mapPtr + _bitsPerScanMap));
			return endPtr - scanPtr;
		}
		Debug_OS_true(((uintptr_t)NULL == scanPtr) || (((uintptr_t)_mapPtr == scanPtr) && !hasMoreSlots()));
		return 0;
	}
};
#endif /* !defined(MIXEDOBJECTSCANNER_HPP_) */
