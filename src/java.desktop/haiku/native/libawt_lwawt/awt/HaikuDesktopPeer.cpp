/*
 * Copyright (c) 2012, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.  Oracle designates this
 * particular file as subject to the "Classpath" exception as provided
 * by Oracle in the LICENSE file that accompanied this code.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */

#include <jni.h>

#include "sun_hawt_HaikuDesktopPeer.h"

#include <Entry.h>
#include <Roster.h>

extern "C" {

/*
 * Class:     sun_hawt_HaikuDesktopPeer
 * Method:    nativeOpen
 * Signature: (Ljava/lang/String;)I
 */
JNIEXPORT jint JNICALL
Java_sun_hawt_HaikuDesktopPeer_nativeOpen
  (JNIEnv *env, jclass clazz, jstring uri)
{
	const char* nativeUri = env->GetStringUTFChars(uri, NULL);
	if (nativeUri == NULL)
		return -1;

	entry_ref ref;
	if (get_ref_for_path("/bin/open", &ref) != B_OK)
		return -1;

	const char* args[] = { "/bin/open", nativeUri, NULL };
	be_roster->Launch(&ref, 2, args);
	env->ReleaseStringUTFChars(uri, nativeUri);
	return 0;
}

}
