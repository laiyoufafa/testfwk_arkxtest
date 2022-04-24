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

#include <napi/native_api.h>
#include <napi/native_node_api.h>
#include <string>
#include <unistd.h>
#include "json.hpp"
#include "common_defines.h"
#include "common_utilities_hpp.h"

namespace OHOS::uitest {
    using namespace nlohmann;
    using namespace std;

    static constexpr size_t NAPI_MAX_STRING_ARG_LENGTH = 1024;
    static constexpr size_t NAPI_MAX_ARG_COUNT = 8;
    // type of unexpected or napi-internal error
    static constexpr napi_status NAPI_ERR = napi_status::napi_generic_failure;
    // the name of property that represents the By object is global seed.
    static constexpr char PROP_IS_SEED[] = "isSeedBy_";
    // the name of property that represents the metadata of the native uitest object
    static constexpr char PROP_METADATA[] = "nativeObjectMetadata_";
    // the name of property that represents the DataType id of the js uitest object
    static constexpr char PROP_TYPE_ID[] = "dataType_";
    // the name of property that represents the bound UiDriver object of the UiComponent object
    static constexpr char PROP_BOUND_DRIVER[] = "boundUiDriver_";
    /**Supported UiComponent attribute types. Ordered by <code>UiAttr</code> definition.*/
    static constexpr TypeId ATTR_TYPES[13] = {INT, STRING, STRING, STRING, STRING, BOOL, BOOL, BOOL, BOOL, BOOL, BOOL, BOOL, BOOL};
    /**Supported By-relative builder native-API names..Ordered by <code>RelMode</code> definition.*/
    static constexpr CStr BY_REL_NAMES_CPP[2] = {"WidgetSelector::AddRearLocator", "WidgetSelector::AddFrontLocator"};
    /**StaticSyncCreator function of 'By', <b>for internal usage only</b> to convert seedBy to new By instance.*/
    static napi_callback gInternalByCreator = nullptr;
    /**The transaction implementer function.*/
    using TransactFuncProto = string (*)(string_view, string_view, string_view);
#ifdef __DOUBLE_FRAMEWORK__
    static TransactFuncProto transactFunc = nullptr;
    static int32_t count = 0;
    static napi_value GetAndIncreaseCount(napi_env env, napi_callback_info info)
    {
        napi_value val;
        napi_create_int32(env, count, &val);
        count++;
        return val;
    }

    static napi_value SetTransactFunc(napi_env env, napi_callback_info info)
    {
        size_t argc = 1;
        napi_value jsThis = nullptr;
        napi_value argv[1] = {0};
        NAPI_CALL(env, napi_get_cb_info(env, info, &argc, argv, &jsThis, nullptr));
        NAPI_ASSERT(env, argc == 1, "need 1 parameter");

        napi_valuetype valueType = napi_undefined;
        napi_typeof(env, argv[0], &valueType);
        NAPI_ASSERT(env, valueType == napi_number, "unexpected non-number parameter");
#if defined(__LP64__)
        napi_get_value_int64(env, argv[0], reinterpret_cast <int64_t *>(&transactFunc));
#else
        napi_get_value_int32(env, argv[0], reinterpret_cast <int32_t*>(&transactFunc));
#endif
        return nullptr;
    }

#else

    // use external setup/transact/disposal callback functions
    extern bool SetupTransactionEnv(string_view token);

    extern string TransactionClientFunc(string_view apiId, string_view caller, string_view params);

    extern void DisposeTransactionEnv();

    static TransactFuncProto transactFunc = TransactionClientFunc;

    /**Lifecycle callback, setup transaction environment, called externally.*/
    static napi_value EnvironmentSetup(napi_env env, napi_callback_info info)
    {
        static auto setUpDone = false;
        if (setUpDone) {
            return nullptr;
        }
        LOG_I("Begin setup transaction environment");
        size_t argc = 1;
        napi_value value = nullptr;
        napi_value argv[1] = {0};
        NAPI_CALL(env, napi_get_cb_info(env, info, &argc, argv, &value, nullptr));
        NAPI_ASSERT(env, argc > 0, "Need session token argument!");
        constexpr size_t bufSize = NAPI_MAX_STRING_ARG_LENGTH;
        char buf[bufSize] = {0};
        size_t strLen = 0;
        NAPI_CALL(env, napi_get_value_string_utf8(env, argv[0], buf, bufSize, &strLen));
        setUpDone = SetupTransactionEnv(string_view(buf, strLen));
        LOG_I("End setup transaction environment, result=%{public}d", setUpDone);
        NAPI_ASSERT(env, setUpDone, "SetupTransactionEnv failed");
        return nullptr;
    }

    /**Lifecycle callback, teardown transaction environment, called externally.*/
    static napi_value EnvironmentTeardown(napi_env env, napi_callback_info info)
    {
        LOG_I("Begin teardown transaction environment");
        DisposeTransactionEnv();
        LOG_I("End teardown transaction environment");
        return nullptr;
    }

#endif

    /**Encapsulates the data objects needed in once api transaction.*/
    struct TransactionData {
        string_view apiId_;
        bool isStaticApi_ = false;
        size_t argc_ = NAPI_MAX_ARG_COUNT;
        napi_value jsThis_ = nullptr;
        napi_value argv_[NAPI_MAX_ARG_COUNT] = {nullptr};
        TypeId argTypes_[NAPI_MAX_ARG_COUNT] = {TypeId::NONE};
        TypeId returnType_ = TypeId::NONE;
        // custom inspector that evaluates the transaction result value and returns napi_error is any.
        function<napi_value(napi_env, napi_value)> resultInspector_ = nullptr;
        // the parcel data fields
        string jsThisParcel_;
        string argvParcel_;
    };

    static napi_value CreateJsException(napi_env env, string_view code, string_view msg)
    {
        napi_value codeValue, msgValue, errorValue;
        napi_create_string_utf8(env, code.data(), NAPI_AUTO_LENGTH, &codeValue);
        napi_create_string_utf8(env, msg.data(), NAPI_AUTO_LENGTH, &msgValue);
        napi_create_error(env, codeValue, msgValue, &errorValue);
        return errorValue;
    }

    /**Set or update object property of string type.*/
    inline static napi_status SetOrUpdateStrProp(napi_env env, napi_value obj, string_view name, string_view value)
    {
        napi_value nv = nullptr;
        NAPI_CALL_BASE(env, napi_create_string_utf8(env, value.data(), value.length(), &nv), NAPI_ERR);
        return napi_set_named_property(env, obj, name.data(), nv);
    }

    /**Set object constructor function to global as attribute.*/
    static napi_status MountJsConstructorToGlobal(napi_env env, TypeId id, napi_value function)
    {
        NAPI_ASSERT_BASE(env, function != nullptr, "Null constructor function", napi_invalid_arg);
        string name = "constructor_" + to_string(id);
        napi_value global = nullptr;
        NAPI_CALL_BASE(env, napi_get_global(env, &global), NAPI_ERR);
        NAPI_CALL_BASE(env, napi_set_named_property(env, global, name.c_str(), function), NAPI_ERR);
        return napi_ok;
    }

    /**Get object constructor function from global as attribute.*/
    static napi_status GetJsConstructorFromGlobal(napi_env env, TypeId id, napi_value* pFunction)
    {
        NAPI_ASSERT_BASE(env, pFunction != nullptr, "Null constructor receiver", napi_invalid_arg);
        string name = "constructor_" + to_string(id);
        napi_value global = nullptr;
        NAPI_CALL_BASE(env, napi_get_global(env, &global), NAPI_ERR);
        NAPI_CALL_BASE(env, napi_get_named_property(env, global, name.c_str(), pFunction), NAPI_ERR);
        return napi_ok;
    }

    /**Marshal object into json, throw error and return false if the object cannot be serialized.*/
    static napi_status MarshalObject(napi_env env, napi_value in, TypeId type, json &out)
    {
        NAPI_ASSERT_BASE(env, in != nullptr, "Illegal null arguments", napi_invalid_arg);
        size_t len = 0;
        bool bValue = false;
        int32_t iValue = 0;
        double fValue = 0;
        constexpr size_t bufSize = NAPI_MAX_STRING_ARG_LENGTH;
        char sValue[bufSize] = {0};
        napi_value oValue = nullptr;
        out[KEY_DATA_TYPE] = type;
        switch (type) {
            case TypeId::BOOL:
                NAPI_CALL_BASE(env, napi_get_value_bool(env, in, &bValue), NAPI_ERR);
                out[KEY_DATA_VALUE] = bValue;
                break;
            case TypeId::INT:
                NAPI_CALL_BASE(env, napi_get_value_int32(env, in, &iValue), NAPI_ERR);
                out[KEY_DATA_VALUE] = iValue;
                break;
            case TypeId::FLOAT:
                NAPI_CALL_BASE(env, napi_get_value_double(env, in, &fValue), NAPI_ERR);
                out[KEY_DATA_VALUE] = fValue;
                break;
            case TypeId::STRING:
                NAPI_CALL_BASE(env, napi_get_value_string_utf8(env, in, sValue, bufSize, &len), NAPI_ERR);
                out[KEY_DATA_VALUE] = string(sValue);
                break;
            default:
                NAPI_CALL_BASE(env, napi_get_named_property(env, in, PROP_METADATA, &oValue), NAPI_ERR);
                NAPI_CALL_BASE(env, napi_get_value_string_utf8(env, oValue, sValue, bufSize, &len), NAPI_ERR);
                out[KEY_DATA_VALUE] = json::parse(string(sValue)); // marshal as json-object
                break;
        }
        return napi_ok;
    }

    /**Unmarshal object from json, throw error and return false if the object cannot be deserialized.*/
    static napi_status UnmarshalObject(napi_env env, const json &in, napi_value *pOut, const TransactionData &tp)
    {
        NAPI_ASSERT_BASE(env, pOut != nullptr && in.contains(KEY_DATA_VALUE), "Illegal arguments", napi_invalid_arg);
        bool bVal;
        int32_t iValue;
        double dVal;
        string sVal;
        uint32_t typeId = in[KEY_DATA_TYPE];
        NAPI_ASSERT_BASE(env, typeId == tp.returnType_, "Illegal result value type", napi_invalid_arg);
        switch (tp.returnType_) {
            case TypeId::BOOL:
                bVal = in[KEY_DATA_VALUE];
                NAPI_CALL_BASE(env, napi_get_boolean(env, bVal, pOut), NAPI_ERR);
                break;
            case TypeId::INT:
                iValue = in[KEY_DATA_VALUE];
                NAPI_CALL_BASE(env, napi_create_int32(env, iValue, pOut), NAPI_ERR);
                break;
            case TypeId::FLOAT:
                dVal = in[KEY_DATA_VALUE];
                NAPI_CALL_BASE(env, napi_create_double(env, dVal, pOut), NAPI_ERR);
                break;
            case TypeId::STRING:
                sVal = in[KEY_DATA_VALUE];
                NAPI_CALL_BASE(env, napi_create_string_utf8(env, sVal.c_str(), sVal.length(), pOut), NAPI_ERR);
                break;
            default:
                // create object instance and bind metadata
                napi_value constructor;
                NAPI_CALL_BASE(env, GetJsConstructorFromGlobal(env, tp.returnType_, &constructor), NAPI_ERR);
                NAPI_CALL_BASE(env, napi_new_instance(env, constructor, 0, nullptr, pOut), NAPI_ERR);
                sVal = in[KEY_DATA_VALUE].dump();
                NAPI_CALL_BASE(env, SetOrUpdateStrProp(env, *pOut, PROP_METADATA, sVal), NAPI_ERR);
                if (tp.returnType_ == COMPONENT) {
                    // bind the the UiDriver that find this UiComponent
                    NAPI_CALL_BASE(env, napi_set_named_property(env, *pOut, PROP_BOUND_DRIVER, tp.jsThis_), NAPI_ERR);
                }
                break;
        }
        return napi_ok;
    }

    /**Generate transaction outgoing arguments-data parcel.*/
    static napi_status MarshalTransactionData(napi_env env, TransactionData &tp)
    {
        LOG_D("Start to marshal transaction parameters, count=%{public}zu", tp.argc_);
        auto paramList = json::array();
        for (size_t idx = 0; idx < tp.argc_; idx++) {
            json paramItem;
            NAPI_CALL_BASE(env, MarshalObject(env, tp.argv_[idx], tp.argTypes_[idx], paramItem), NAPI_ERR);
            paramList.emplace_back(paramItem);
            // erase parameters which are expected not to be used in the reset transaction procedure
            tp.argv_[idx] = nullptr;
        }
        tp.argc_ = 0;
        tp.argvParcel_ = paramList.dump();
        if (!tp.isStaticApi_ && tp.jsThis_ != nullptr) {
            json receiverItem;
            LOG_D("Start to serialize jsThis");
            napi_value typeProp = nullptr;
            NAPI_CALL_BASE(env, napi_get_named_property(env, tp.jsThis_, PROP_TYPE_ID, &typeProp), NAPI_ERR);
            uint32_t id = TypeId::NONE;
            NAPI_CALL_BASE(env, napi_get_value_uint32(env, typeProp, &id), NAPI_ERR);
            NAPI_CALL_BASE(env, MarshalObject(env, tp.jsThis_, static_cast<TypeId>(id), receiverItem), NAPI_ERR);
            tp.jsThisParcel_ = receiverItem[KEY_DATA_VALUE].dump();
        } else {
            tp.jsThisParcel_ = "{}";
        }
        return napi_ok;
    }

    /**Evaluate and convert transaction result-data parcel to object. Return the exception raised during the
     * transaction if any, else return the result object. */
    template<bool kReturnMultiple = false>
    static napi_value UnmarshalTransactResult(napi_env env, TransactionData &tp, string_view resultParcel)
    {
        napi_value result = nullptr;
        NAPI_CALL(env, napi_get_undefined(env, &result));
        LOG_D("Start to Unmarshal transaction results: '%{public}s'", resultParcel.data());
        json reply = json::parse(resultParcel);
        NAPI_ASSERT(env, reply.contains(KEY_UPDATED_CALLER) && reply.contains(KEY_RESULT_VALUES), "Fields missing");
        if (reply.contains(KEY_EXCEPTION)) {
            json error = reply[KEY_EXCEPTION];
            string codeStr = error.contains(KEY_CODE) ? error[KEY_CODE] : "unknown";
            string msgStr = error.contains(KEY_MESSAGE) ? error[KEY_MESSAGE] : "unknown";
            return CreateJsException(env, codeStr, msgStr);
        }
        if (!tp.isStaticApi_ && tp.jsThis_ != nullptr) {
            LOG_D("Begin to update jsThis");
            const string meta = reply[KEY_UPDATED_CALLER].dump();
            NAPI_CALL(env, SetOrUpdateStrProp(env, tp.jsThis_, PROP_METADATA, meta));
        }
        if (tp.returnType_ == TypeId::NONE) {
            return result;
        }
        json resultValues = reply[KEY_RESULT_VALUES];
        const size_t resultCount = resultValues.size();
        napi_value objects;
        NAPI_CALL(env, napi_create_array_with_length(env, resultCount, &objects));
        LOG_D("Begin to deserialize result, count=%{public}zu", resultCount);
        for (size_t idx = 0; idx < resultCount; idx++) {
            napi_value obj = nullptr;
            NAPI_CALL(env, UnmarshalObject(env, resultValues.at(idx), &obj, tp));
            NAPI_CALL(env, napi_set_element(env, objects, idx, obj));
        }
        if constexpr(kReturnMultiple) {
            result = objects;
        } else if (resultCount > 0) {
            NAPI_CALL(env, napi_get_element(env, objects, 0, &result));
        }
        return tp.resultInspector_ != nullptr ? tp.resultInspector_(env, result) : result;
    }

    /**Call api with parameters out, wait for and return result value or throw raised exception.*/
    template<bool kReturnMultiple = false>
    static napi_value TransactSync(napi_env env, TransactionData &tp)
    {
        LOG_D("TargetApi=%{public}s", tp.apiId_.data());
        NAPI_CALL(env, MarshalTransactionData(env, tp));
        auto resultParcel = transactFunc(tp.apiId_.data(), tp.jsThisParcel_.c_str(), tp.argvParcel_.c_str());
        auto resultValue = UnmarshalTransactResult<kReturnMultiple>(env, tp, resultParcel);
        auto isError = false;
        NAPI_CALL(env, napi_is_error(env, resultValue, &isError));
        if (isError) {
            NAPI_CALL(env, napi_throw(env, resultValue));
            NAPI_CALL(env, napi_get_undefined(env, &resultValue)); // return undefined it's error
        }
        return resultValue;
    }

    /**Encapsulates the data objects needed for async transaction.*/
    struct AsyncTransactionData {
        TransactionData data_;
        string resultParcel_;
        napi_async_work asyncWork_ = nullptr;
        napi_deferred deferred_ = nullptr;
        napi_ref jsThisRef_ = nullptr;
    };

    /**Call api with parameters out, return a promise.*/
    template<bool kReturnMultiple = false>
    static napi_value TransactAsync(napi_env env, TransactionData &tp)
    {
        static constexpr uint32_t refCount = 1;
        LOG_D("TargetApi=%{public}s", tp.apiId_.data());
        NAPI_CALL(env, MarshalTransactionData(env, tp));
        napi_value resName;
        NAPI_CALL(env, napi_create_string_latin1(env, __FUNCTION__, NAPI_AUTO_LENGTH, &resName));
        auto atd = new AsyncTransactionData();
        atd->data_ = tp;
        napi_value promise;
        NAPI_CALL(env, napi_create_promise(env, &(atd->deferred_), &promise));
        NAPI_CALL(env, napi_create_reference(env, tp.jsThis_, refCount, &(atd->jsThisRef_)));
        napi_create_async_work(env, nullptr, resName,
            [](napi_env env, void *data) {
                auto atd = reinterpret_cast<AsyncTransactionData *>(data);
                // NOT:: use 'auto&' rather than 'auto', or the result will be set to copy-constructed temp-object
                auto &tp = atd->data_;
                atd->resultParcel_ = transactFunc(tp.apiId_.data(), tp.jsThisParcel_.c_str(), tp.argvParcel_.c_str());
            },
            [](napi_env env, napi_status status, void *data) {
                auto atd = reinterpret_cast<AsyncTransactionData *>(data);
                napi_get_reference_value(env, atd->jsThisRef_, &(atd->data_.jsThis_));
                auto resultValue = UnmarshalTransactResult<kReturnMultiple>(env, atd->data_, atd->resultParcel_);
                napi_delete_reference(env, atd->jsThisRef_);
                auto isError = false;
                napi_is_error(env, resultValue, &isError);
                if (isError) {
                    napi_reject_deferred(env, atd->deferred_, resultValue);
                } else {
                    napi_resolve_deferred(env, atd->deferred_, resultValue);
                }
                delete atd;
            },
            (void *) atd, &(atd->asyncWork_));
        napi_queue_async_work(env, atd->asyncWork_);
        return promise;
    }

    /**Extract transaction data from callback_info arguments and check the data types.*/
    static napi_status ExtractTransactionData(napi_env env, napi_callback_info info, size_t minArgc,
                                              const vector<TypeId> &argTypes, TransactionData &tp)
    {
        tp.argc_ = NAPI_MAX_ARG_COUNT; // extract as much argument as possible
        NAPI_CALL_BASE(env, napi_get_cb_info(env, info, &(tp.argc_), tp.argv_, &(tp.jsThis_), nullptr), NAPI_ERR);
        if (!tp.isStaticApi_) {
            NAPI_ASSERT_BASE(env, tp.jsThis_ != nullptr, "Null jsThis!", napi_invalid_arg);
        } else {
            NAPI_CALL_BASE(env, napi_get_undefined(env, &(tp.jsThis_)), NAPI_ERR);
        }
        // check argument count and types
        NAPI_ASSERT_BASE(env, tp.argc_ >= minArgc, "Illegal argument count", napi_invalid_arg);
        NAPI_ASSERT_BASE(env, argTypes.size() >= tp.argc_, "Illegal argument count", napi_invalid_arg);
        napi_valuetype valueType;
        for (size_t idx = 0; idx < tp.argc_; idx++) {
            tp.argTypes_[idx] = argTypes.at(idx);
            NAPI_CALL_BASE(env, napi_typeof(env, tp.argv_[idx], &valueType), NAPI_ERR);
            const bool isNullOrUndefined = valueType == napi_null || valueType == napi_undefined;
            NAPI_ASSERT_BASE(env, !isNullOrUndefined, "Null argument", napi_invalid_arg);
            const TypeId dt = argTypes.at(idx);
            if (dt == INT || dt == FLOAT) {
                NAPI_ASSERT_BASE(env, valueType == napi_number, "Illegal argument type", napi_number_expected);
            } else if (dt == BOOL) {
                NAPI_ASSERT_BASE(env, valueType == napi_boolean, "Illegal argument type", napi_boolean_expected);
            } else if (dt == STRING) {
                NAPI_ASSERT_BASE(env, valueType == napi_string, "Illegal argument type", napi_string_expected);
            } else {
                NAPI_ASSERT_BASE(env, valueType == napi_object, "Illegal argument type", napi_object_expected);
                // check the typeId property (set during object initialization)
                napi_value idProp = nullptr;
                int32_t idValue = TypeId::NONE;
                NAPI_CALL_BASE(env, napi_get_named_property(env, tp.argv_[idx], PROP_TYPE_ID, &idProp), NAPI_ERR);
                NAPI_CALL_BASE(env, napi_get_value_int32(env, idProp, &idValue), NAPI_ERR);
                NAPI_ASSERT_BASE(env, idValue == dt, "Illegal argument type", napi_invalid_arg);
            }
        }
        return napi_ok;
    }

    /**Generic template of functions that run asynchronously.*/
    template<CStr kNativeApiId, TypeId kReturnType, bool kReturnMultiple, TypeId... kArgTypes>
    static napi_value GenericAsyncFunc(napi_env env, napi_callback_info info)
    {
        static_assert(!string_view(kNativeApiId).empty(), "Native function name cannot be empty");
        static constexpr size_t argc = sizeof...(kArgTypes);
        vector<TypeId> types = {};
        if constexpr(argc > 0) {
            types = {kArgTypes...};
        }
        TransactionData tp {.apiId_ = kNativeApiId, .returnType_ = kReturnType};
        NAPI_CALL(env, ExtractTransactionData(env, info, argc, types, tp));
        return TransactAsync<kReturnMultiple>(env, tp);
    }

    /**Template of JS wrapper-object static creator functions.*/
    template<CStr kNativeApiId, TypeId kReturnType, TypeId... kArgTypes>
    static napi_value StaticSyncCreator(napi_env env, napi_callback_info info)
    {
        static_assert(!string_view(kNativeApiId).empty(), "Native function name cannot be empty");
        static constexpr size_t argc = sizeof...(kArgTypes);
        vector<TypeId> types = {};
        if constexpr(argc > 0) {
            types = {kArgTypes...};
        }
        TransactionData tp = {.apiId_=kNativeApiId, .isStaticApi_ = true, .returnType_=kReturnType};
        if (info == nullptr) {
            // allow call without arguments
            tp.argc_ = 0;
            tp.jsThis_ = nullptr;
        } else {
            NAPI_CALL(env, ExtractTransactionData(env, info, argc, types, tp));
        }
        return TransactSync(env, tp);
    }

    /**Convert global seed-By (for program syntactic sugar purpose) to new By object.*/
    static napi_status EnsureNonSeedBy(napi_env env, TransactionData &tp)
    {
        bool hasProp = false;
        NAPI_CALL_BASE(env, napi_has_named_property(env, tp.jsThis_, PROP_IS_SEED, &hasProp), NAPI_ERR);
        if (hasProp) {
            NAPI_ASSERT_BASE(env, gInternalByCreator != nullptr, "Static By-Creator is null", NAPI_ERR);
            LOG_D("Convert seedBy to new instance");
            tp.jsThis_ = gInternalByCreator(env, nullptr);
        }
        return napi_ok;
    }

    /**Conversion between value and string, using the builtin JSON methods.*/
    static napi_status ValueStringConvert(napi_env env, napi_value in, napi_value *out, TypeId inType, TypeId outType)
    {
        if (in == nullptr || out == nullptr) {
            return napi_invalid_arg;
        }
        if (inType == outType) {
            *out = in;
            return napi_ok;
        }
        napi_value global = nullptr;
        napi_value jsonProp = nullptr;
        napi_value jsonFunc = nullptr;
        NAPI_CALL_BASE(env, napi_get_global(env, &global), NAPI_ERR);
        NAPI_CALL_BASE(env, napi_get_named_property(env, global, "JSON", &jsonProp), NAPI_ERR);
        if (outType == TypeId::STRING) {
            NAPI_CALL_BASE(env, napi_get_named_property(env, jsonProp, "stringify", &jsonFunc), NAPI_ERR);
        } else {
            NAPI_CALL_BASE(env, napi_get_named_property(env, jsonProp, "parse", &jsonFunc), NAPI_ERR);
        }
        napi_value argv[1] = {in};
        NAPI_CALL_BASE(env, napi_call_function(env, jsonProp, jsonFunc, 1, argv, out), NAPI_ERR);
        return napi_ok;
    }

    /**Template for plain attribute By-builder functions.*/
    template<UiAttr kAttr>
    static napi_value ByAttrBuilder(napi_env env, napi_callback_info info)
    {
        static constexpr auto attrName = ATTR_NAMES[kAttr];
        static constexpr auto attrType = ATTR_TYPES[kAttr];
        // incoming args: testValue, matchPattern(optional)
        TransactionData tp = {.apiId_= "WidgetSelector::AddMatcher"};
        NAPI_CALL(env, ExtractTransactionData(env, info, 0, {attrType, TypeId::INT}, tp));
        if (attrType == TypeId::BOOL && tp.argc_ == 0) {
            // for attribute of type bool, the input-arg is default to true: By.enabled()===By.enabled(true)
            NAPI_CALL(env, napi_get_boolean(env, true, &(tp.argv_[INDEX_ZERO])));
            tp.argTypes_[INDEX_ZERO] = TypeId::BOOL;
            tp.argc_++;
        }
        NAPI_ASSERT(env, tp.argc_ >= 1, "Insufficient argument"); // require attribute testValue
        NAPI_CALL(env, EnsureNonSeedBy(env, tp));
        if (tp.argc_ == 1) {
            // fill-in default match pattern
            NAPI_CALL(env, napi_create_int32(env, ValueMatchRule::EQ, &(tp.argv_[INDEX_TWO])));
            tp.argTypes_[INDEX_TWO] = TypeId::INT;
            tp.argc_++;
        } else {
            // move match pattern to index2
            tp.argv_[INDEX_TWO] = tp.argv_[INDEX_ONE];
            tp.argTypes_[INDEX_TWO] = TypeId::INT;
        }
        // move testValue to index1, [convert it from any type to string]
        NAPI_CALL(env, ValueStringConvert(env, tp.argv_[INDEX_ZERO], &(tp.argv_[INDEX_ONE]), attrType, TypeId::STRING));
        tp.argTypes_[INDEX_ONE] = TypeId::STRING;
        // fill-in attribute name to index0
        NAPI_CALL(env, napi_create_string_utf8(env, attrName, NAPI_AUTO_LENGTH, &(tp.argv_[INDEX_ZERO])));
        tp.argTypes_[INDEX_ZERO] = TypeId::STRING;
        tp.argc_++;
        TransactSync(env, tp);
        // return jsThis, which has updated its metaData in the transaction
        return tp.jsThis_;
    }

    /**Template for relative By-builder functions.*/
    template<RelMode kRelMode>
    static napi_value ByRelBuilder(napi_env env, napi_callback_info info)
    {
        static constexpr auto apiId = BY_REL_NAMES_CPP[kRelMode];
        // incoming args: relative-By
        TransactionData tp = {.apiId_=apiId};
        NAPI_CALL(env, ExtractTransactionData(env, info, 1, {TypeId::BY}, tp));
        NAPI_CALL(env, EnsureNonSeedBy(env, tp));
        TransactSync(env, tp);
        // return jsThis, which has updated its metaData in the transaction
        return tp.jsThis_;
    }

    /**Template for all UiComponent-attribute-getter functions, which forward invocation to bound UiDriver api.*/
    template<UiAttr kAttr>
    static napi_value ComponentAttrGetter(napi_env env, napi_callback_info info)
    {
        static constexpr auto attrName = ATTR_NAMES[kAttr];
        static constexpr auto attrType = ATTR_TYPES[kAttr];
        // retrieve attribute value as string and convert to target type
        TransactionData tp = {.apiId_= "UiDriver::GetWidgetAttribute", .returnType_=TypeId::STRING};
        tp.resultInspector_ = [](napi_env env, napi_value result) -> napi_value {
            napi_valuetype type = napi_null;
            if (result != nullptr) {
                NAPI_CALL(env, napi_typeof(env, result, &type));
            }
            if (type == napi_null || type == napi_undefined) {
                return result;
            } else {
                napi_value convertedResult = result;
                NAPI_CALL(env, ValueStringConvert(env, result, &convertedResult, TypeId::STRING, attrType));
                return convertedResult;
            }
        };
        NAPI_CALL(env, ExtractTransactionData(env, info, 0, {}, tp));
        // get the holding uiDriver which has found and will operate me
        napi_value uiDriver = nullptr;
        NAPI_CALL(env, napi_get_named_property(env, tp.jsThis_, PROP_BOUND_DRIVER, &uiDriver));
        NAPI_ASSERT(env, uiDriver != nullptr, "UiDriver not found for UiComponent");
        // Transformation:: {widget.getId() ===> uiDriver.GetWidgetAttribute(widget,"id")}
        tp.argv_[0] = tp.jsThis_;
        tp.argTypes_[0] = TypeId::COMPONENT;
        tp.jsThis_ = uiDriver;
        tp.argc_++;
        // add attributeName parameter
        NAPI_CALL(env, napi_create_string_utf8(env, attrName, NAPI_AUTO_LENGTH, &(tp.argv_[1])));
        tp.argTypes_[1] = TypeId::STRING;
        tp.argc_++;
        return TransactAsync(env, tp);
    }

    /**Template for all UiComponent touch functions, which forward invocation to bound UiDriver api, return void.*/
    template<WidgetOp kAction>
    static napi_value ComponentToucher(napi_env env, napi_callback_info info)
    {
        TransactionData tp = {.apiId_= "UiDriver::PerformWidgetOperate", .returnType_=TypeId::NONE};
        NAPI_CALL(env, ExtractTransactionData(env, info, 0, {}, tp));
        // get the holding uiDriver which has found and will operate me
        napi_value uiDriver = nullptr;
        NAPI_CALL(env, napi_get_named_property(env, tp.jsThis_, PROP_BOUND_DRIVER, &uiDriver));
        NAPI_ASSERT(env, uiDriver != nullptr, "UiDriver not found for UiComponent");
        // Transformation:: {widget.click() ===> uiDriver.PerformAction(widget,CLICK)}
        tp.argv_[0] = tp.jsThis_;
        tp.argTypes_[0] = TypeId::COMPONENT;
        tp.jsThis_ = uiDriver;
        tp.argc_++;
        // add actionCode parameter
        NAPI_CALL(env, napi_create_int32(env, kAction, &(tp.argv_[1])));
        tp.argTypes_[1] = TypeId::INT;
        tp.argc_++;
        return TransactAsync(env, tp);
    }

    /**Generic template for all UiComponent functions, which forward invocation to bound UiDriver api.*/
    template<CStr kNativeApiId, TypeId kReturnType, TypeId... kArgTypes>
    static napi_value GenericComponentFunc(napi_env env, napi_callback_info info)
    {
        static_assert(!string_view(kNativeApiId).empty(), "Native function name cannot be empty");
        static constexpr size_t argc = sizeof...(kArgTypes);
        vector<TypeId> types = {};
        if constexpr(argc > 0) {
            types = {kArgTypes...};
        }
        TransactionData tp = {.apiId_= kNativeApiId, .returnType_=kReturnType};
        NAPI_CALL(env, ExtractTransactionData(env, info, argc, types, tp));
        // get the holding uiDriver which has found and will operate me
        napi_value uiDriver = nullptr;
        NAPI_CALL(env, napi_get_named_property(env, tp.jsThis_, PROP_BOUND_DRIVER, &uiDriver));
        NAPI_ASSERT(env, uiDriver != nullptr, "UiDriver not found for UiComponent");
        // Transformation:: {widget.func(arg0,arg1) ===> uiDriver.func(widget,arg0,arg1)}, need right shift args
        for (size_t idx = tp.argc_; idx > 0; idx--) {
            tp.argv_[idx] = tp.argv_[idx - 1];
            tp.argTypes_[idx] = tp.argTypes_[idx - 1];
        }
        tp.argv_[0] = tp.jsThis_;
        tp.argTypes_[0] = TypeId::COMPONENT;
        tp.jsThis_ = uiDriver;
        tp.argc_++;
        return TransactAsync(env, tp);
    }

    /**Template for all UiDriver-key-action functions, return void.*/
    template<UiKey kKey>
    static napi_value UiDriverKeyOperator(napi_env env, napi_callback_info info)
    {
        TransactionData tp = {.apiId_= "UiDriver::TriggerKey"};
        NAPI_CALL(env, ExtractTransactionData(env, info, 0, {TypeId::INT}, tp));
        if constexpr(kKey != UiKey::GENERIC) {
            // for named key, add keyCode parameter
            tp.argc_ = 1;
            NAPI_CALL(env, napi_create_int32(env, kKey, &(tp.argv_[0])));
            tp.argTypes_[0] = TypeId::INT;
        } else {
            // for generic key, require keyCode from argv
            NAPI_ASSERT(env, tp.argc_ >= 1, "Need keyCode argument");
            for (size_t idx = tp.argc_; idx > 0; idx--) {
                tp.argv_[idx] = tp.argv_[idx - 1];
                tp.argTypes_[idx] = tp.argTypes_[idx - 1];
            }
            NAPI_CALL(env, napi_create_int32(env, kKey, &(tp.argv_[0])));
            tp.argTypes_[0] = TypeId::INT;
            tp.argc_++;
        }
        return TransactAsync(env, tp);
    }

    /**Find and assert component matching given selector exist. <b>(async function)</b>*/
    static napi_value UiDriverComponentExistAsserter(napi_env env, napi_callback_info info)
    {
        TransactionData tp = {.apiId_= "UiDriver::FindWidgets", .returnType_=TypeId::COMPONENT};
        NAPI_CALL(env, ExtractTransactionData(env, info, 1, {TypeId::BY}, tp));
        tp.resultInspector_ = [](napi_env env, napi_value result) -> napi_value {
            napi_valuetype type = napi_null;
            if (result != nullptr) {
                NAPI_CALL(env, napi_typeof(env, result, &type));
            }
            if (type == napi_null || type == napi_undefined) {
                return CreateJsException(env, "ComponentExistAssertionFailure", "ComponentNotExist");
            } else {
                return result;
            }
        };
        return TransactAsync(env, tp);
    }

    /**Template for all UiDriver single-pointer-based touch functions (generic-click/swipe/drag), return void.*/
    template<PointerOp kAction>
    static napi_value SinglePointToucher(napi_env env, napi_callback_info info)
    {
        static constexpr auto isGenericSwipe = kAction >= PointerOp::SWIPE_P && kAction <= PointerOp::DRAG_P;
        constexpr size_t argC = isGenericSwipe ? 4 : 2;
        TransactionData tp {};
        NAPI_CALL(env, ExtractTransactionData(env, info, argC,
            {TypeId::INT, TypeId::INT, TypeId::INT, TypeId::INT}, tp));
        if constexpr (isGenericSwipe) {
            tp.apiId_ = "UiDriver::PerformGenericSwipe";
        } else {
            tp.apiId_ = "UiDriver::PerformGenericClick";
        }
        // add action type as 1st parameter (right-shift provided argv, do from right to left to avoid overwriting)
        for (size_t idx = tp.argc_; idx > 0; idx--) {
            tp.argv_[idx] = tp.argv_[idx - 1];
            tp.argTypes_[idx] = tp.argTypes_[idx - 1];
        }
        NAPI_CALL(env, napi_create_uint32(env, kAction, &(tp.argv_[0])));
        tp.argc_++;
        tp.argTypes_[tp.argc_] = TypeId::INT;
        return TransactAsync(env, tp);
    }

    /**Template of function that initialize <b>unbound</b> uitest js wrapper-objects.*/
    template<TypeId kObjectType>
    static napi_value JsObjectInitializer(napi_env env, napi_callback_info info)
    {
        TransactionData tp;
        NAPI_CALL(env, ExtractTransactionData(env, info, 0, {}, tp));
        // set DataType property to jsThis
        napi_value typeId = nullptr;
        NAPI_CALL(env, napi_create_int32(env, kObjectType, &typeId));
        NAPI_CALL(env, napi_set_named_property(env, tp.jsThis_, PROP_TYPE_ID, typeId));
        return tp.jsThis_;
    }

    /**Exports uitest js wrapper-classes and its global constructor.*/
    static napi_value ExportClass(napi_env env, napi_value exports, string_view name, TypeId type,
                                  napi_callback initializer, const napi_property_descriptor *methods, size_t num)
    {
        NAPI_ASSERT(env, exports != nullptr && initializer != nullptr && methods != nullptr, "Illegal export params");
        // define class, provide the js-class members(property) and initializer.
        napi_value ctor = nullptr;
        NAPI_CALL(env, napi_define_class(env, name.data(), name.length(), initializer, nullptr, num, methods, &ctor));
        NAPI_CALL(env, napi_set_named_property(env, exports, name.data(), ctor));
        NAPI_CALL(env, MountJsConstructorToGlobal(env, type, ctor));

        if (type == TypeId::BY) {
            // export global By-seed object (unbound, no metadata)
            napi_value bySeed = nullptr;
            NAPI_CALL(env, napi_new_instance(env, ctor, 0, nullptr, &bySeed));
            NAPI_CALL(env, SetOrUpdateStrProp(env, bySeed, PROP_IS_SEED, "true")); // see seed-mark property
            NAPI_CALL(env, napi_set_named_property(env, exports, "BY", bySeed));
        }
        return exports;
    }

    /**Exports 'MatchValue' enumeration.*/
    static napi_value ExportMatchPattern(napi_env env, napi_value exports)
    {
        napi_value propMatchPattern;
        napi_value propEquals;
        napi_value propContains;
        napi_value propStartsWith;
        napi_value propEndsWith;
        NAPI_CALL(env, napi_create_object(env, &propMatchPattern));
        NAPI_CALL(env, napi_create_int32(env, ValueMatchRule::EQ, &propEquals));
        NAPI_CALL(env, napi_create_int32(env, ValueMatchRule::CONTAINS, &propContains));
        NAPI_CALL(env, napi_create_int32(env, ValueMatchRule::STARTS_WITH, &propStartsWith));
        NAPI_CALL(env, napi_create_int32(env, ValueMatchRule::ENDS_WITH, &propEndsWith));
        NAPI_CALL(env, napi_set_named_property(env, propMatchPattern, "EQUALS", propEquals));
        NAPI_CALL(env, napi_set_named_property(env, propMatchPattern, "CONTAINS", propContains));
        NAPI_CALL(env, napi_set_named_property(env, propMatchPattern, "STARTS_WITH", propStartsWith));
        NAPI_CALL(env, napi_set_named_property(env, propMatchPattern, "ENDS_WITH", propEndsWith));
        NAPI_CALL(env, napi_set_named_property(env, exports, "MatchPattern", propMatchPattern));
        return exports;
    }

    /**Exports 'By' class definition and member functions.*/
    static napi_value ExportBy(napi_env env, napi_value exports)
    {
        static constexpr char cppCreator[] = "WidgetSelector::<init>";
        napi_property_descriptor methods[] = {
            DECLARE_NAPI_FUNCTION(ATTR_NAMES[UiAttr::ID], ByAttrBuilder<UiAttr::ID>),
            DECLARE_NAPI_FUNCTION(ATTR_NAMES[UiAttr::TEXT], ByAttrBuilder<UiAttr::TEXT>),
            DECLARE_NAPI_FUNCTION(ATTR_NAMES[UiAttr::KEY], ByAttrBuilder<UiAttr::KEY>),
            DECLARE_NAPI_FUNCTION(ATTR_NAMES[UiAttr::TYPE], ByAttrBuilder<UiAttr::TYPE>),
            DECLARE_NAPI_FUNCTION(ATTR_NAMES[UiAttr::ENABLED], ByAttrBuilder<UiAttr::ENABLED>),
            DECLARE_NAPI_FUNCTION(ATTR_NAMES[UiAttr::FOCUSED], ByAttrBuilder<UiAttr::FOCUSED>),
            DECLARE_NAPI_FUNCTION(ATTR_NAMES[UiAttr::SELECTED], ByAttrBuilder<UiAttr::SELECTED>),
            DECLARE_NAPI_FUNCTION(ATTR_NAMES[UiAttr::CLICKABLE], ByAttrBuilder<UiAttr::CLICKABLE>),
            DECLARE_NAPI_FUNCTION(ATTR_NAMES[UiAttr::LONG_CLICKABLE], ByAttrBuilder<UiAttr::LONG_CLICKABLE>),
            DECLARE_NAPI_FUNCTION(ATTR_NAMES[UiAttr::SCROLLABLE], ByAttrBuilder<UiAttr::SCROLLABLE>),
            DECLARE_NAPI_FUNCTION(ATTR_NAMES[UiAttr::CHECKABLE], ByAttrBuilder<UiAttr::CHECKABLE>),
            DECLARE_NAPI_FUNCTION(ATTR_NAMES[UiAttr::CHECKED], ByAttrBuilder<UiAttr::CHECKED>),
            DECLARE_NAPI_FUNCTION("isBefore", ByRelBuilder<RelMode::IS_BEFORE>),
            DECLARE_NAPI_FUNCTION("isAfter", ByRelBuilder<RelMode::IS_AFTER>)
        };
        static constexpr size_t num = sizeof(methods) / sizeof(methods[0]);
        static constexpr napi_callback initializer = JsObjectInitializer<TypeId::BY>;
        // StaticSyncCreator for internal usage, not exposed to 'By' class
        gInternalByCreator = StaticSyncCreator<cppCreator, BY>;
        return ExportClass(env, exports, "By", TypeId::BY, initializer, methods, num);
    }

    /**Exports 'UiComponent' class definition and member functions.*/
    napi_value ExportUiComponent(napi_env env, napi_value exports)
    {
        // UiComponent method calls will be forwarded to the bound UiDriver object
        static constexpr char cppInput[] = "UiDriver::InputText";
        static constexpr char cppSearch[] = "UiDriver::ScrollSearch";
        static constexpr char cppDragTo[] = "UiDriver::DragWidgetToAnother";
        static constexpr napi_property_descriptor methods[] = {
            DECLARE_NAPI_FUNCTION("getId", ComponentAttrGetter<UiAttr::ID>),
            DECLARE_NAPI_FUNCTION("getText", ComponentAttrGetter<UiAttr::TEXT>),
            DECLARE_NAPI_FUNCTION("getKey", ComponentAttrGetter<UiAttr::KEY>),
            DECLARE_NAPI_FUNCTION("getType", ComponentAttrGetter<UiAttr::TYPE>),
            DECLARE_NAPI_FUNCTION("isEnabled", ComponentAttrGetter<UiAttr::ENABLED>),
            DECLARE_NAPI_FUNCTION("isFocused", ComponentAttrGetter<UiAttr::FOCUSED>),
            DECLARE_NAPI_FUNCTION("isSelected", ComponentAttrGetter<UiAttr::SELECTED>),
            DECLARE_NAPI_FUNCTION("isClickable", ComponentAttrGetter<UiAttr::CLICKABLE>),
            DECLARE_NAPI_FUNCTION("isLongClickable", ComponentAttrGetter<UiAttr::LONG_CLICKABLE>),
            DECLARE_NAPI_FUNCTION("isScrollable", ComponentAttrGetter<UiAttr::SCROLLABLE>),
            DECLARE_NAPI_FUNCTION("isCheckable", ComponentAttrGetter<UiAttr::CHECKABLE>),
            DECLARE_NAPI_FUNCTION("isChecked", ComponentAttrGetter<UiAttr::CHECKED>),
            DECLARE_NAPI_FUNCTION("click", ComponentToucher<WidgetOp::CLICK>),
            DECLARE_NAPI_FUNCTION("longClick", ComponentToucher<WidgetOp::LONG_CLICK>),
            DECLARE_NAPI_FUNCTION("doubleClick", ComponentToucher<WidgetOp::DOUBLE_CLICK>),
            DECLARE_NAPI_FUNCTION("inputText", (GenericComponentFunc<cppInput, TypeId::NONE, TypeId::STRING>)),
            DECLARE_NAPI_FUNCTION("scrollSearch", (GenericComponentFunc<cppSearch, TypeId::COMPONENT, TypeId::BY>)),
            DECLARE_NAPI_FUNCTION("dragTo", (GenericComponentFunc<cppDragTo, TypeId::NONE, TypeId::COMPONENT>))
        };
        static constexpr size_t num = sizeof(methods) / sizeof(methods[0]);
        static constexpr napi_callback init = JsObjectInitializer<TypeId::COMPONENT>;
        return ExportClass(env, exports, "UiComponent", TypeId::COMPONENT, init, methods, num);
    }

    /**Exports 'UiDriver' class definition and member functions.*/
    static napi_value ExportUiDriver(napi_env env, napi_value exports)
    {
        static constexpr char cppCreator[] = "UiDriver::<init>";
        static constexpr char cppDelay[] = "UiDriver::DelayMs";
        static constexpr char cppFinds[] = "UiDriver::FindWidgets";
        static constexpr char cppCap[] = "UiDriver::TakeScreenCap";
        static constexpr napi_property_descriptor methods[] = {
            DECLARE_NAPI_STATIC_FUNCTION("create", (StaticSyncCreator<cppCreator, TypeId::DRIVER>)),
            DECLARE_NAPI_FUNCTION("delayMs", (GenericAsyncFunc<cppDelay, TypeId::NONE, false, TypeId::INT>)),
            DECLARE_NAPI_FUNCTION("findComponents", (GenericAsyncFunc<cppFinds, TypeId::COMPONENT, true, TypeId::BY>)),
            DECLARE_NAPI_FUNCTION("findComponent", (GenericAsyncFunc<cppFinds, TypeId::COMPONENT, false, TypeId::BY>)),
            DECLARE_NAPI_FUNCTION("screenCap", (GenericAsyncFunc<cppCap, TypeId::BOOL, false, TypeId::STRING>)),
            DECLARE_NAPI_FUNCTION("assertComponentExist", UiDriverComponentExistAsserter),
            DECLARE_NAPI_FUNCTION("pressBack", UiDriverKeyOperator<UiKey::BACK>),
            DECLARE_NAPI_FUNCTION("triggerKey", UiDriverKeyOperator<UiKey::GENERIC>),
            // raw coordinate based action methods
            DECLARE_NAPI_FUNCTION("click", SinglePointToucher<PointerOp::CLICK_P>),
            DECLARE_NAPI_FUNCTION("longClick", SinglePointToucher<PointerOp::LONG_CLICK_P>),
            DECLARE_NAPI_FUNCTION("doubleClick", SinglePointToucher<PointerOp::DOUBLE_CLICK_P>),
            DECLARE_NAPI_FUNCTION("swipe", SinglePointToucher<PointerOp::SWIPE_P>),
            DECLARE_NAPI_FUNCTION("drag", SinglePointToucher<PointerOp::DRAG_P>),
        };
        static constexpr size_t num = sizeof(methods) / sizeof(methods[0]);
        static constexpr napi_callback init = JsObjectInitializer<TypeId::DRIVER>;
        return ExportClass(env, exports, "UiDriver", TypeId::DRIVER, init, methods, num);
    }

    napi_value Export(napi_env env, napi_value exports)
    {
        LOG_I("Begin export uitest apis");
#ifdef __DOUBLE_FRAMEWORK__
        napi_property_descriptor props[] = {
            DECLARE_NAPI_STATIC_FUNCTION("setTransactFunc", SetTransactFunc),
            DECLARE_NAPI_STATIC_FUNCTION("getAndIncreaseCount", GetAndIncreaseCount)
        };
        NAPI_CALL(env, napi_define_properties(env, exports, sizeof(props)/ sizeof(props[0]), props));
#else
        // export transaction-environment lifecycle callbacks
        napi_property_descriptor props[] = {
            DECLARE_NAPI_STATIC_FUNCTION("setup", EnvironmentSetup),
            DECLARE_NAPI_STATIC_FUNCTION("teardown", EnvironmentTeardown)
        };
        NAPI_CALL(env, napi_define_properties(env, exports, sizeof(props)/ sizeof(props[0]), props));
#endif
        if (ExportMatchPattern(env, exports) == nullptr) {
            return nullptr;
        }
        if (ExportBy(env, exports) == nullptr) {
            return nullptr;
        }
        if (ExportUiComponent(env, exports) == nullptr) {
            return nullptr;
        }
        if (ExportUiDriver(env, exports) == nullptr) {
            return nullptr;
        }
        LOG_I("End export uitest apis");
        return exports;
    }

    static napi_module module = {
        .nm_version = 1,
        .nm_flags = 0,
        .nm_filename = nullptr,
        .nm_register_func = Export,
        .nm_modname = "uitest",
        .nm_priv = ((void *) 0),
        .reserved = {0},
    };

    extern "C" __attribute__((constructor)) void RegisterModule(void)
    {
        napi_module_register(&module);
    }
}

#ifndef __DOUBLE_FRAMEWORK__
// put register functions out of namespace to ensure C-linkage
extern const char _binary_uitest_exporter_js_start[];
extern const char _binary_uitest_exporter_js_end[];
extern const char _binary_uitest_exporter_abc_start[];
extern const char _binary_uitest_exporter_abc_end[];

extern "C" __attribute__((visibility("default")))
void NAPI_uitest_GetJSCode(const char **buf, int *bufLen)
{
    if (buf != nullptr) {
        *buf = _binary_uitest_exporter_js_start;
    }
    if (bufLen != nullptr) {
        *bufLen = _binary_uitest_exporter_js_end - _binary_uitest_exporter_js_start;
    }
}

extern "C" __attribute__((visibility("default")))
void NAPI_uitest_GetABCCode(const char **buf, int *bufLen)
{
    if (buf != nullptr) {
        *buf = _binary_uitest_exporter_abc_start;
    }
    if (bufLen != nullptr) {
        *bufLen = _binary_uitest_exporter_abc_end - _binary_uitest_exporter_abc_start;
    }
}
#endif