/* Android Modem Status Client API
 *
 * Copyright (C) Intel 2012
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

package com.intel.internal.telephony;

public enum ModemStatus {
    NONE(0),
    DOWN(1),
    UP(2),
    DEAD(4),
    NFLUSH(8),
    ALL(0xFF);

    private int value;

    private ModemStatus(int value) {
        this.value = value;
    }

    public int getValue() {
        return this.value;
    }
}
