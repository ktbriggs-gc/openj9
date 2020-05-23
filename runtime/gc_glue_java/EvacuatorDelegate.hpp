
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

#ifndef EVACUATORDELEGATE_HPP_
#define EVACUATORDELEGATE_HPP_

#include "j9.h"
#include "j9cfg.h"
#include "j9javaaccessflags.h"
#include "j9nonbuilder.h"
#include "omr.h"

#include "EnvironmentStandard.hpp"
#include "EvacuatorBase.hpp"
#include "GCExtensionsBase.hpp"
#include "Forge.hpp"
#include "ForwardedHeader.hpp"
#include "IndexableObjectScanner.hpp"
#include "LinkedObjectScanner.hpp"
#include "MixedObjectScanner.hpp"
#include "ObjectModel.hpp"

#if defined(EVACUATOR_DEBUG)
#define EVACUATOR_DEBUG_DELEGATE_REFERENCES EVACUATOR_DEBUG_DELEGATE_BASE
#define EVACUATOR_DEBUG_DELEGATE_REFERENCE_SCAN_ROOT 0
#define EVACUATOR_DEBUG_DELEGATE_REFERENCE_SCAN_HEAP 1
#define EVACUATOR_DEBUG_DELEGATE_REFERENCE_CLEAR 2
#define EVACUATOR_DEBUG_DELEGATE_REFERENCE_ADD 3
#endif /* defined(EVACUATOR_DEBUG) */

class MM_Evacuator;
class MM_EvacuatorController;
class MM_Scavenger;
class MM_EvacuatorRootScanner;

class MM_EvacuatorDelegate
{
/*
 * Data members
 */
private:
	MM_EnvironmentStandard *_env;
	MM_Scavenger *_controller;
	MM_Evacuator *_evacuator;
	MM_EvacuatorRootScanner * _rootScanner;
	GC_ObjectModel * _objectModel;
	MM_Forge *_forge;
	bool _cycleCleared;

protected:
public:
	/**
	 * Controller maintains a volatile bitset of delegate flags, evacuators mutate and test
	 * these to control evacuation stages of each gc cycle. Evacuator delegate may define
	 * flags as required to share this information across evacuators during gc cycles.
	 *
	 * @see MM_EvacuatorController::max_evacuator_public_flag (public bit mask is 0xffffffff)
	 */
	enum {
		shouldScavengeFinalizableObjects = 1
		, shouldScavengeUnfinalizedObjects = 2
		, shouldScavengeSoftReferenceObjects = 4
		, shouldScavengeWeakReferenceObjects = 8
		, shouldScavengePhantomReferenceObjects = 16
#if defined(J9VM_GC_FINALIZATION)
		, finalizationRequired = 32
#endif /* J9VM_GC_FINALIZATION */
	};

/*
 * Function members
 */
private:
	GC_ObjectScanner *getReferenceObjectScanner(omrobjectptr_t objectptr, void *objectScannerState, uintptr_t flags);
	GC_ObjectScanner *getOwnableSynchronizerObjectScanner(omrobjectptr_t objectptr, void *objectScannerState, uintptr_t flags);
	GC_ObjectScanner *getPointerArrayObjectScanner(omrobjectptr_t objectptr, void *objectScannerState, uintptr_t flags);

protected:
public:
	/**
	 * Evacuator calls this when it instantiates the delegate to bind controller-evacuator-delegate. This
	 * binding persists over the delegate's lifetime.
	 *
	 * @param evacuator the MM_Evacuator instance to bind delegate to
	 * @param forge points to system memory allocator
	 * @controller points to the evacuator controller
	 */
	bool initialize(MM_Evacuator *evacuator, MM_Forge *forge, MM_EvacuatorController *controller);

	/**
	 * This is called when the OMR vm is shut down, to release resources held by the delegate
	 */
	void tearDown();

	/**
	 * This is called from the controller before activating any evacuator instances to allow the
	 * delegate to set up evacuator flags for the evacuation cycle.
	 *
	 * @param env environment for calling thread
	 * @return preset evacuator flags for the cycle
	 */
	static uintptr_t prepareForEvacuation(MM_EnvironmentBase *env);

	/**
	 * Evacuator calls this when it starts starts work in an evacuation cycle. This binds the evacuator
	 * gc thread (environment) to the evacuator-delegate for the duration of the cycle. This method must
	 * be implemented in EvacuatorDelegate.cpp, as MM_Evacutor is inaccessible here. The implementation
	 * must set MM_EvacuatorDelegate::_env to the environment bound to the evacuator at this time.
	 */
	void cycleStart(); /* { _env = evacuator->getEnvironment(); } */

	void cycleEnd();

	/**
	 * Evacuator calls this to instantiate an object scanner within space provided by objectScannerState
	 *
	 * @param worker the calling evacuator
	 * @param objectptr the object to be scanned
	 * @param objectScannerState points to space to instantiate the object scanner into
	 * @param flags to be set in the object scanner
	 * @return a pointer to the object scanner
	 */
	GC_ObjectScanner *
	getObjectScanner(omrobjectptr_t objectptr, void *objectScannerState, uintptr_t flags)
	{
		/* object class must have proper eye catcher */
		Debug_MM_true((UDATA)0x99669966 == J9GC_J9OBJECT_CLAZZ(objectptr, _env)->eyecatcher);
		Debug_MM_true(GC_ObjectScanner::isHeapScan(flags) ^ GC_ObjectScanner::isRootScan(flags));
		GC_ObjectScanner *objectScanner = NULL;

		GC_ObjectModel::ScanType scanType = _objectModel->getScanType(objectptr);
		switch(scanType) {
		case GC_ObjectModel::SCAN_MIXED_OBJECT:
			objectScanner = GC_MixedObjectScanner::newInstance(_env, objectptr, objectScannerState, flags);
			break;
		case GC_ObjectModel::SCAN_MIXED_OBJECT_LINKED:
			objectScanner = GC_LinkedObjectScanner::newInstance(_env, objectptr, objectScannerState, flags);
			break;
		case GC_ObjectModel::SCAN_ATOMIC_MARKABLE_REFERENCE_OBJECT:
		case GC_ObjectModel::SCAN_CLASS_OBJECT:
		case GC_ObjectModel::SCAN_CLASSLOADER_OBJECT:
			objectScanner = GC_MixedObjectScanner::newInstance(_env, objectptr, objectScannerState, flags);
			break;
		case GC_ObjectModel::SCAN_REFERENCE_MIXED_OBJECT:
			objectScanner = getReferenceObjectScanner(objectptr, objectScannerState, flags);
			break;
		case GC_ObjectModel::SCAN_OWNABLESYNCHRONIZER_OBJECT:
			objectScanner = getOwnableSynchronizerObjectScanner(objectptr, objectScannerState, flags);
			break;
		case GC_ObjectModel::SCAN_POINTER_ARRAY_OBJECT:
			objectScanner = getPointerArrayObjectScanner(objectptr, objectScannerState, flags | GC_ObjectScanner::indexableObjectNoSplit);
			break;
		case GC_ObjectModel::SCAN_PRIMITIVE_ARRAY_OBJECT:
			break;
		default:
			Assert_GC_true_with_message2(_env, (UDATA)0x99669966 == J9GC_J9OBJECT_CLAZZ(objectptr, _env)->eyecatcher,
					"Bad header %p for object pointer %p\n", J9GC_J9OBJECT_CLAZZ(objectptr, _env), objectptr);
			Assert_GC_true_with_message2(_env, false, "Bad scan type %d for object pointer %p\n", (intptr_t)scanType, objectptr);
		}

		return objectScanner;
	}

	GC_IndexableObjectScanner *getSplitPointerArrayObjectScanner(omrobjectptr_t objectptr, void *objectScannerState, uintptr_t splitIndex, uintptr_t splitAmount, uintptr_t flags);

	bool
	isIndexablePointerArray(omrobjectptr_t object)
	{
		return (OBJECT_HEADER_SHAPE_POINTERS == J9GC_CLASS_SHAPE(J9GC_J9OBJECT_CLAZZ(object, _env)));
	}

	bool
	isIndexablePointerArray(MM_ForwardedHeader *forwardedHeader)
	{
		return (OBJECT_HEADER_SHAPE_POINTERS == J9GC_CLASS_SHAPE(_objectModel->getPreservedClass(forwardedHeader)));
	}

	fomrobject_t *getIndexableDataBounds(omrobjectptr_t indexableObject, uintptr_t *numberOfElements);

	bool hasClearable() { return !_cycleCleared; }

	bool objectHasIndirectObjectsInNursery(omrobjectptr_t objectptr);

	bool scanIndirectObjects(omrobjectptr_t objectptr);

	void scanRoots();

	void scanClearable();

	void rescanThreadSlots();

	void flushForWaitState();

	void flushForEndCycle();

	MM_EvacuatorDelegate()
		: _env(NULL)
		, _controller(NULL)
		, _evacuator(NULL)
		, _rootScanner(NULL)
		, _objectModel(NULL)
		, _forge(NULL)
		, _cycleCleared(false)
	{ }

#if defined(EVACUATOR_DEBUG)
	bool
	isValidObject(omrobjectptr_t objectptr)
	{
		J9Class *clazz = (J9Class *)J9GC_J9OBJECT_CLAZZ(objectptr, _env);
		return (uintptr_t)0x99669966 == clazz->eyecatcher;
	}
	void debugValidateObject(omrobjectptr_t objectptr);
	void debugValidateObject(MM_ForwardedHeader *forwardedHeader);
	const char *
	debugGetClassname(omrobjectptr_t objectptr, char *buffer, uintptr_t bufferLength)
	{
		J9UTF8 *classNameStruct = J9ROMCLASS_CLASSNAME(((J9Class*)(((J9Object *)objectptr)->clazz & ~(uintptr_t)0xff))->romClass);
		uintptr_t classNameLength = OMR_MIN(classNameStruct->length, bufferLength - 1);
		for (uintptr_t i = 0; i < classNameLength; i += 1) {
			buffer[i] = (char)(classNameStruct->data[i]);
		}
		buffer[classNameLength] = 0;
		return buffer;
	}
	void
	debugDelegateReference(uintptr_t op, omrobjectptr_t reference, omrobjectptr_t referent, uintptr_t referenceObjectType, bool referentMustBeCleared,
			bool isObjectInNewSpace, bool referentMustBeMarked, bool shouldScavenge)
	{
		OMRPORT_ACCESS_FROM_ENVIRONMENT(_env);
		char oname[32], rname[32];
		char t[] = {'?','W','S','P'};
		const char *s[] = {" ROOT"," HEAP","CLEAR","  ADD"};
		const char *f = "";
		if (NULL != referent) {
			MM_ForwardedHeader forwardedHeader(referent, _env->compressObjectReferences());
			if (forwardedHeader.isForwardedPointer()) {
				referent = forwardedHeader.getForwardedObject();
				f = "*";
			}
		} else {
			f="~";
		}
		omrtty_printf("reference %s[%c]: %llx %s%llx %c%c%c%c %-31s %s\n", s[op], t[referenceObjectType/J9AccClassReferenceWeak], (uint64_t)reference, f,
				(uint64_t)referent, (referentMustBeCleared?'C':'c'), (isObjectInNewSpace?'N':'n'), (referentMustBeMarked?'M':'m'), (shouldScavenge?'S':'s'),
				debugGetClassname(reference, &oname[0], 32), (NULL != referent) ? debugGetClassname(referent, &rname[0], 32) : "nil");
	}
#endif /* defined(EVACUATOR_DEBUG) */
};
#endif /* EVACUATORDELEGATE_HPP_ */
