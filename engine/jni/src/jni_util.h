// Copyright 2020-2023 The Defold Foundation
// Copyright 2014-2020 King
// Copyright 2009-2014 Ragnar Svensson, Christian Murray
// Licensed under the Defold License version 1.0 (the "License"); you may not use
// this file except in compliance with the License.
//
// You may obtain a copy of the License, together with FAQs at
// https://www.defold.com/license
//
// Unless required by applicable law or agreed to in writing, software distributed
// under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
// CONDITIONS OF ANY KIND, either express or implied. See the License for the
// specific language governing permissions and limitations under the License.


#ifndef DM_JNI_H
#define DM_JNI_H

// https://docs.oracle.com/javase/8/docs/technotes/guides/jni/spec/functions.html#Set_type_Field_routines

#include <stdint.h>
#include <jni.h>

#if defined(_WIN32)
    #include <Windows.h>
    #include <Dbghelp.h>
    #include <excpt.h>
#endif

namespace dmJNI
{
    // Jni helper functions
    jclass GetClass(JNIEnv* env, const char* basecls, const char* clsname);
    jclass GetFieldType(JNIEnv* env, jobject obj, jfieldID fieldID);

    void SetObject(JNIEnv* env, jobject obj, jfieldID field, jobject value);
    void SetObjectDeref(JNIEnv* env, jobject obj, jfieldID field, jobject value);
    void SetBoolean(JNIEnv* env, jobject obj, jfieldID field, jboolean value);
    void SetByte(JNIEnv* env, jobject obj, jfieldID field, jbyte value);
    void SetChar(JNIEnv* env, jobject obj, jfieldID field, jchar value);
    void SetShort(JNIEnv* env, jobject obj, jfieldID field, jshort value);
    void SetInt(JNIEnv* env, jobject obj, jfieldID field, jint value);
    void SetLong(JNIEnv* env, jobject obj, jfieldID field, jlong value);
    void SetFloat(JNIEnv* env, jobject obj, jfieldID field, jfloat value);
    void SetDouble(JNIEnv* env, jobject obj, jfieldID field, jdouble value);
    void SetString(JNIEnv* env, jobject obj, jfieldID field, const char* value);

    // Requires that the enum class has a "static Enum fromValue(int value)" function.
    void SetEnum(JNIEnv* env, jobject obj, jfieldID field, int value);

    bool    GetBoolean(JNIEnv* env, jobject obj, jfieldID field);
    uint8_t GetByte(JNIEnv* env, jobject obj, jfieldID field);
    char    GetChar(JNIEnv* env, jobject obj, jfieldID field);
    int16_t GetShort(JNIEnv* env, jobject obj, jfieldID field);
    int32_t GetInt(JNIEnv* env, jobject obj, jfieldID field);
    int64_t GetLong(JNIEnv* env, jobject obj, jfieldID field);
    float   GetFloat(JNIEnv* env, jobject obj, jfieldID field);
    double  GetDouble(JNIEnv* env, jobject obj, jfieldID field);
    char*   GetString(JNIEnv* env, jobject obj, jfieldID field);

    // Requires that the enum class has a "int getValue()" function.
    int     GetEnum(JNIEnv* env, jobject obj, jfieldID field);

    jbooleanArray   CreateBooleanArray(JNIEnv* env, const bool* data, uint32_t data_count);
    jbyteArray      CreateByteArray(JNIEnv* env, const uint8_t* data, uint32_t data_count);
    jcharArray      CreateCharArray(JNIEnv* env, const char* data, uint32_t data_count);
    jshortArray     CreateShortArray(JNIEnv* env, const int16_t* data, uint32_t data_count);
    jintArray       CreateIntArray(JNIEnv* env, const int32_t* data, uint32_t data_count);
    jlongArray      CreateLongArray(JNIEnv* env, const int64_t* data, uint32_t data_count);
    jfloatArray     CreateFloatArray(JNIEnv* env, const float* data, uint32_t data_count);
    jdoubleArray    CreateDoubleArray(JNIEnv* env, const double* data, uint32_t data_count);

    void EnableDefaultSignalHandlers(JavaVM* vm);
    void EnableSignalHandlers(void* ctx, void (*callback)(int signal, void* ctx));
    void DisableSignalHandlers();

    // Used to enable a jni context for a small period of time
    bool IsContextAdded(JNIEnv* env);
    void AddContext(JNIEnv* env);
    void RemoveContext(JNIEnv* env);

    struct ScopedString
    {
        JNIEnv* m_Env;
        jstring m_JString;
        const char* m_String;
        ScopedString(JNIEnv* env, jstring str)
        : m_Env(env)
        , m_JString(str)
        , m_String(env->GetStringUTFChars(str, JNI_FALSE))
        {
        }
        ~ScopedString()
        {
            if (m_String)
            {
                m_Env->ReleaseStringUTFChars(m_JString, m_String);
            }
        }
    };

    struct SignalContextScope
    {
        JNIEnv* m_Env;
        SignalContextScope(JNIEnv* env) : m_Env(env)
        {
            AddContext(m_Env);
        }
        ~SignalContextScope()
        {
            RemoveContext(m_Env);
        }
    };

#if defined(_WIN32)
    LONG WINAPI ExceptionHandler(EXCEPTION_POINTERS* ptr);

    #define DM_JNI_GUARD_SCOPE_BEGIN() \
        __try { \

    #define DM_JNI_GUARD_SCOPE_END(...) \
        } __except (dmJNI::ExceptionHandler(GetExceptionInformation())) { \
            __VA_ARGS__ \
        }

#else
    #define DM_JNI_GUARD_SCOPE_BEGIN() { int _scope_result = 0;
    #define DM_JNI_GUARD_SCOPE_END(...) \
            if (_scope_result) { \
                __VA_ARGS__ \
            } \
        }

#endif

    // Caller has to free() the memory
    char* GenerateCallstack(char* buffer, uint32_t buffer_length);

// for testing
// Pass in a string containing any of the SIG* numbers
    void TestSignalFromString(const char* signal);

    void PrintString(JNIEnv* env, jstring string);
}

#endif
