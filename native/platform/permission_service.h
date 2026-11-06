// Android's "permission" service, which system_server normally provides. Native services (AudioFlinger
// among them) ask it whether a caller may do something, e.g. MODIFY_AUDIO_SETTINGS for setParameters;
// with system_server gone they wait for it forever. Rondo is the whole system on its firmware, so it
// answers: granted. Written as a plain BBinder that handles the one transaction itself
// (IPermissionController::CHECK_PERMISSION_TRANSACTION), so nothing depends on how MediaTek built that
// interface's classes. Include between aosp_begin.h and aosp_end.h.
#pragma once
#include <binder/Binder.h>
#include <binder/IPCThreadState.h>
#include <binder/IServiceManager.h>
#include <binder/Parcel.h>
#include <utils/String16.h>

namespace rondo {

class PermissionService : public android::BBinder {
 public:
  // registers it with servicemanager as "permission"; needs a running binder thread pool
  static bool publish() {
    android::sp<android::IBinder> self =
        new (::operator new(sizeof(PermissionService) + 256)) PermissionService();
    return android::defaultServiceManager()->addService(android::String16("permission"), self) == android::NO_ERROR;
  }

 protected:
  android::status_t onTransact(uint32_t code, const android::Parcel& data, android::Parcel* reply,
                               uint32_t flags) override {
    if (code != android::IBinder::FIRST_CALL_TRANSACTION)  // CHECK_PERMISSION_TRANSACTION
      return android::BBinder::onTransact(code, data, reply, flags);
    data.enforceInterface(android::String16("android.os.IPermissionController"));
    data.readString16();  // the permission
    data.readInt32();     // pid
    data.readInt32();     // uid
    reply->writeInt32(0);  // no exception
    reply->writeInt32(1);  // granted
    return android::NO_ERROR;
  }
};

}  // namespace rondo
