#include <string>
#include <stdio.h>
#include <stdlib.h>
#include <jvmti.h>
#include <string.h>
#include <map>
#include "java_crw_demo.h"

using namespace std;

#define arraysize(a) (sizeof(a)/sizeof(*(a)))

void warnx(const char *fmt, ...) {
  va_list ap;

  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  fflush(stderr);
  va_end(ap);
}

void errx(int code, const char *fmt, ...) {
  va_list ap;

  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  fflush(stderr);
  va_end(ap);
  exit(code);
}

class Monitor {
 public:
  inline explicit Monitor(jvmtiEnv* jvmti, jrawMonitorID monitor)
      : jvmti_(jvmti), monitor_(monitor)
  {}
        
  jvmtiEnv* jvmti() {
    return jvmti_;
  }

  jrawMonitorID monitor() {
    return monitor_;
  }

 private:
  jvmtiEnv* jvmti_;
  jrawMonitorID monitor_;
};

class Lock {
 public:
  inline explicit Lock(Monitor* monitor) {
    lock(monitor->jvmti(), monitor->monitor());
  }

  inline explicit Lock(jvmtiEnv* jvmti, jrawMonitorID monitor) {
    lock(jvmti, monitor);
  }

  inline ~Lock() {
    jvmtiError error = jvmti_->RawMonitorExit(monitor_);
    if (error != JVMTI_ERROR_NONE)
      errx(3, "Failed to unlock monitor");
  }

 private:
  void lock(jvmtiEnv* jvmti, jrawMonitorID monitor) {
    jvmti_ = jvmti;
    monitor_ = monitor;

    jvmtiError error = jvmti_->RawMonitorEnter(monitor_);
    if (error != JVMTI_ERROR_NONE)
      errx(3, "Failed to lock monitor");
  }

  jvmtiEnv* jvmti_;
  jrawMonitorID monitor_;
};

#define HELPER_CLASS "HeapsterHelper"
#define HELPER_FIELD_ISREADY "isReady"

class Heapster {
 public:
  static const uint32_t kHashTableSize;

  struct Site {
    Site(Site* _next, long _hash, int _nframes, jvmtiFrameInfo* frames)
        : next(_next),
          hash(_hash),
          nframes(_nframes),
          stack(new pair<jmethodID, jlocation>[_nframes]),
          num_allocs(0),
          num_bytes(0) {}

    ~Site() {
      delete stack;
    }

    Site*                       next;
    long                        hash;
    int                         nframes;
    pair<jmethodID, jlocation>* stack;

    // Stats.
    int num_allocs;
    int num_bytes;
  };

  static Heapster* instance;

  // * Static JNI hooks.
  static void JNI_NewObject(
      JNIEnv* env, jclass klass,
      jthread thread, jobject o) {
    instance->NewObject(env, klass, thread, o);
  }

  // * Static JVMTI hooks
  static void JNICALL JVMTI_VMStart(jvmtiEnv *jvmti, JNIEnv *env) {
    instance->VMStart(env);
  }
   
  static void JNICALL JVMTI_VMDeath(jvmtiEnv *jvmti, JNIEnv *env) {
    // heapster->VMDeath();
  }
   
  static void JNICALL JVMTI_ObjectFree(jvmtiEnv *jvmti, jlong tag) {
    // heapster->ObjectFree(tag);
  }

  static void JNICALL JVMTI_ClassFileLoadHook(
      jvmtiEnv *jvmti, JNIEnv* env,
      jclass class_being_redefined, jobject loader,
      const char* name, jobject protection_domain,
      jint class_data_len, const unsigned char* class_data,
      jint* new_class_data_len, unsigned char** new_class_data) {
    instance->ClassFileLoadHook(
        jvmti, env, class_being_redefined, loader, name,
        protection_domain, class_data_len, class_data,
        new_class_data_len, new_class_data);
  }

  // * Instance methods.

  Heapster(jvmtiEnv* jvmti)
      : jvmti_(jvmti), raw_monitor_(NULL), monitor_(NULL),
        class_count_(0), vm_started_(false) {
    Setup();
  }

  ~Heapster() {
    // TODO: deallocate raw_monitor.
    // TODO: deallocate sites table.
    delete monitor_;
  }

  void VMStart(JNIEnv* env) {
    jclass klass;
    static JNINativeMethod registry[] = {
      { (char*)"_newObject",
        (char*)"(Ljava/lang/Object;Ljava/lang/Object;)V",
        (void *)&Heapster::JNI_NewObject }
    };

    klass = env->FindClass(HELPER_CLASS);
    if ((klass = env->FindClass(HELPER_CLASS)) == NULL)
      errx(3, "Failed to find the heapster helper class (%s)\n", HELPER_CLASS);

    { // Register natives.
      Lock l(monitor_);
      vm_started_ = true;

      // TODO: Does this need to be inside of the lock?
      if (env->RegisterNatives(klass, registry, arraysize(registry)) != 0)
        errx(3, "Failed to register natives for %s", HELPER_CLASS);
    }

    // Set the static field to hint the helper.
    jfieldID field = env->GetStaticFieldID(klass, HELPER_FIELD_ISREADY, "I");
    if (field == NULL)
      errx(3, "Failed to get %s field\n", HELPER_FIELD_ISREADY);

    env->SetStaticIntField(klass, field, 1);
  }

  void JNICALL ClassFileLoadHook(
      jvmtiEnv *jvmti, JNIEnv* env,
      jclass class_being_redefined, jobject loader,
      const char* name, jobject protection_domain,
      jint class_data_len, const unsigned char* class_data,
      jint* new_class_data_len, unsigned char** new_class_data) {
    // This is where the magic rewriting happens.

    char* classname;
    if (name == NULL) {
      classname = java_crw_demo_classname(class_data, class_data_len, NULL);
      if (classname == NULL)
        errx(3, "Failed to find classname\n");
    } else {
      if ((classname = strdup(name)) == NULL)
        errx(3, "malloc failed\n");
    }

    // Ignore the helper class.
    if (strcmp(classname, HELPER_CLASS) == 0)
      return;

    int class_num;
    bool is_system_class;
    {
      Lock l(monitor_);
      class_num = class_count_++;
      is_system_class = !vm_started_;
    }

    // The big magic: rewrite the class with our instrumentation.
    unsigned char *new_image = NULL;
    long new_length = 0L;

    java_crw_demo(
      class_num,
      classname,
      class_data,
      class_data_len,
      is_system_class ? 1 : 0,
      (char*)HELPER_CLASS,
      (char*)("L" HELPER_CLASS ";"),
      NULL, NULL,
      NULL, NULL,
      (char*)"newObject", (char*)"(Ljava/lang/Object;)V",
      (char*)"newObject", (char*)"(Ljava/lang/Object;)V",
      &new_image,
      &new_length,
      NULL, NULL);

    if (new_length > 0L) {
      // Success. We now need to allocate it with the JVMTI allocator,
      // copy the definition there, and set the corresponding
      // pointers.
      void *bufp;
      Assert(jvmti_->Allocate(new_length, (unsigned char **)&bufp),
             "failed to allocate buffer for new classfile");

      memcpy(bufp, new_image, new_length);
      *new_class_data_len = (jint)new_length;
      *new_class_data = (unsigned char*)bufp;
    }

    if (new_image != NULL)
      free(new_image);
  }

  void NewObject(JNIEnv* env, jclass klass, jthread thread, jobject o) {
    jvmtiFrameInfo frames[100];
    jint nframes;

    // Subtract 2 stack frames to start outside of our own code. TODO:
    // use AsyncGetStackTrace?
    jvmtiError error =
        jvmti_->GetStackTrace(
            thread, 2, arraysize(frames),
            frames, &nframes);

    if (error == JVMTI_ERROR_WRONG_PHASE)
      return;

    // This hash function was adapted from Google perftools.
    long h = 0;
    for (int i = 0; i < nframes; i++) {
      h += frames[i].location;
      h += h << 10;
      h ^= h >> 6;
    }
    h += h << 3;
    h ^= h >> 11;

    uint32_t bucket = h % kHashTableSize;
    Site* s = sites_[bucket];
    for (; s != NULL; s = s->next) {
      if (s->hash == h && s->nframes == nframes) {
        int i = 0;
        for (; i < nframes; i++) {
          if (frames[i].method != s->stack[i].first ||
              frames[i].location != s->stack[i].second) {
            break;
          }
        }

        if (i == nframes)
          break;
      }
    }

    if (s == NULL)
      s = new Site(sites_[bucket], h, nframes, frames);

    jlong size;
    Assert(jvmti_->GetObjectSize(o, &size),
           "failed to get size of object");

    s->num_allocs++;
    s->num_bytes += size;
  }

 private:
  void Assert(jvmtiError err, string message) {
    char* strerr;

    if (err == JVMTI_ERROR_NONE)
      return;

    jvmti_->GetErrorName(err, &strerr);
    errx(3, "jvmti error %s: %s\n", strerr, message.c_str());
  }

  void Setup() {
    jvmtiCapabilities c; 
    memset(&c, 0, sizeof(c));
    c.can_generate_all_class_hook_events = 1;
    c.can_tag_objects                    = 1;
    c.can_generate_object_free_events    = 1;
    Assert(jvmti_->AddCapabilities(&c), "failed to add capabilities");

    jvmtiEventCallbacks cb; 
    memset(&cb, 0, sizeof(cb));
    cb.VMStart           = &Heapster::JVMTI_VMStart;
    cb.VMDeath           = &Heapster::JVMTI_VMDeath;
    cb.ObjectFree        = &Heapster::JVMTI_ObjectFree;
    cb.ClassFileLoadHook = &Heapster::JVMTI_ClassFileLoadHook;
    Assert(jvmti_->SetEventCallbacks(&cb, (jint)sizeof(cb)),
           "failed to set callbacks");

    jvmtiEvent events[] = {
      JVMTI_EVENT_VM_START, JVMTI_EVENT_VM_DEATH,
      JVMTI_EVENT_CLASS_FILE_LOAD_HOOK,
      JVMTI_EVENT_OBJECT_FREE
    };

    for (uint32_t i = 0; i < arraysize(events); i++) {
      Assert(jvmti_->SetEventNotificationMode(JVMTI_ENABLE, events[i], NULL),
             "failed to set event notification mode");
    }

    Assert(jvmti_->CreateRawMonitor("heapster state", &raw_monitor_),
           "failed to create heapster monitor");

    monitor_ = new Monitor(jvmti_, raw_monitor_);

    // Set up allocation site table.
    sites_ = new Site*[kHashTableSize];
    memset(sites_, 0, sizeof(Site*) * kHashTableSize);
  }

  jvmtiEnv*     jvmti_;
  jrawMonitorID raw_monitor_;
  Monitor*      monitor_;
  Site**        sites_;

  int  class_count_;
  bool vm_started_;
};

// Same hash table size as TCMalloc.
const uint32_t Heapster::kHashTableSize = 179999;
Heapster* Heapster::instance = NULL;

// This instantiates a singleton for the above heapster class, which
// is used in subsequent hooks.
JNIEXPORT jint JNICALL Agent_OnLoad(JavaVM* jvm, char* options, void* _unused) {
  jvmtiEnv* jvmti = NULL;

  if ((jvm->GetEnv((void**)&jvmti, JVMTI_VERSION_1_0)) != JNI_OK ||
      jvmti == NULL) {
    fprintf(stderr, "unable to access JVMTI version 1\n");
    exit(1);
  }

  Heapster::instance = new Heapster(jvmti);

  return JNI_OK;
}