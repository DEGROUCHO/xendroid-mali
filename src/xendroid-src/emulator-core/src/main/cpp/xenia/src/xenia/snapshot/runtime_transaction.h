#ifndef XENIA_SNAPSHOT_RUNTIME_TRANSACTION_H_
#define XENIA_SNAPSHOT_RUNTIME_TRANSACTION_H_
#include <mutex>
#include "xenia/snapshot/state_file.h"
namespace xe::snapshot {
// Backend owns every guest/host writer. Stop must abort/release partial barriers
// on failure. Validate must parse ALL sections without runtime side effects.
// Apply cannot run guest threads and must leave the barrier held on failure.
class RuntimeBackend {
 public:
  virtual ~RuntimeBackend()=default;
  virtual Identity identity() const=0;
  virtual bool CanSnapshot(std::string& reason) const=0;
  virtual bool Stop(std::string& error)=0;
  virtual void Resume() noexcept=0;
  virtual bool Capture(Image& image,std::string& error)=0;
  virtual bool Validate(const Image& image,std::string& error) const=0;
  virtual bool Apply(const Image& image,std::string& error)=0;
  // Called while frozen if rollback also failed. Must leave a safe stopped
  // runtime, so Resume cannot execute a partially restored title.
  virtual void FailStopped(const std::string& reason) noexcept=0;
};
class RuntimeTransaction {
 public:
  explicit RuntimeTransaction(RuntimeBackend& backend):backend_(backend){}
  bool Save(const std::filesystem::path& path,std::string& error) {
    std::unique_lock lock(operation_,std::try_to_lock);
    if(!lock.owns_lock()){error="snapshot operation already running";return false;}
    if(!backend_.CanSnapshot(error)||!backend_.Stop(error))return false;
    ResumeGuard resume(backend_);Image image;image.identity=backend_.identity();
    if(!backend_.Capture(image,error)||image.identity!=backend_.identity()||!backend_.Validate(image,error))return false;
    std::vector<uint8_t> bytes;if(!Encode(image,bytes,error))return false;
    return AtomicWrite(path,bytes,error);
  }
  bool Restore(const std::filesystem::path& path,std::string& error) {
    std::unique_lock lock(operation_,std::try_to_lock);
    if(!lock.owns_lock()){error="snapshot operation already running";return false;}
    if(!backend_.CanSnapshot(error))return false;
    // Preflight runs before Stop: bad files must not pause or tear down a title.
    std::vector<uint8_t> bytes;Image desired;
    if(!ReadFile(path,bytes,error)||!Decode(bytes,backend_.identity(),desired,error)||!backend_.Validate(desired,error))return false;
    if(!backend_.Stop(error))return false;
    ResumeGuard resume(backend_);
    if(desired.identity!=backend_.identity()){error="runtime changed during snapshot preflight";return false;}
    Image rollback;rollback.identity=backend_.identity();
    if(!backend_.Capture(rollback,error)||!backend_.Validate(rollback,error))return false;
    if(backend_.Apply(desired,error))return true;
    const std::string original=error;std::string rollback_error;
    if(backend_.Apply(rollback,rollback_error)){error="restore aborted; previous runtime restored: "+original;return false;}
    error="restore and rollback failed; runtime stopped: "+original+"; "+rollback_error;
    backend_.FailStopped(error);resume.Disarm();return false;
  }
 private:
  class ResumeGuard {
   public: explicit ResumeGuard(RuntimeBackend& b):backend_(&b){}
    ~ResumeGuard(){if(backend_)backend_->Resume();}
    void Disarm(){backend_=nullptr;}
   private:RuntimeBackend* backend_;
  };
  RuntimeBackend& backend_;std::mutex operation_;
};
}
#endif
