/*
 * Copyright (c) 2021-2022 Huawei Device Co., Ltd.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

import assertNull from './assertNull';
import assertClose from './assertClose';
import assertContain from './assertContain';
import assertLess from './assertLess';
import assertLarger from './assertLarger';
import assertFail from './assertFail';
import assertUndefined from './assertUndefined';
import assertFalse from './assertFalse';
import assertInstanceOf from './assertInstanceOf';
import assertThrowError from './assertThrowError';

class ExpectExtend {
    constructor(attr) {
        this.id = attr.id;
        this.matchers = {};
    }

    extendsMatchers() {
        this.matchers.assertNull = assertNull;
        this.matchers.assertClose = assertClose;
        this.matchers.assertContain = assertContain;
        this.matchers.assertLess = assertLess;
        this.matchers.assertLarger = assertLarger;
        this.matchers.assertFail = assertFail;
        this.matchers.assertUndefined = assertUndefined;
        this.matchers.assertFalse = assertFalse;
        this.matchers.assertInstanceOf = assertInstanceOf;
        this.matchers.assertThrowError = assertThrowError;
    }

    init(coreContext) {
        this.coreContext = coreContext;
        this.extendsMatchers();
        const expectService = this.coreContext.getDefaultService('expect');
        expectService.addMatchers(this.matchers);
    }

    apis() {
        return {
            'expect': function (actualValue) {
                return this.coreContext.getDefaultService('expect').expect(actualValue);
            }
        };
    }
}

export default ExpectExtend;
