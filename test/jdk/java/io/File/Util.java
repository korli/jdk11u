/*
 * Copyright (c) 2012, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
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

import java.io.File;

/**
 * Utility class used by tests for java.io.File.
 */

class Util {
    private Util() { }

    /**
     * Returns {@code true} if the Java virtual machine is running with
     * privileges (loosely defined here as have write access to /etc/passwd).
     */
    static boolean isPrivileged() {
        String osname = System.getProperty("os.name");
        if (osname.startsWith("Windows"))
            return false;
        if (osname.startsWith("Haiku"))
            return true;

        return (new File("/etc/passwd")).canWrite();
    }
}

