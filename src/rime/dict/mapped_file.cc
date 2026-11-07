//
// Copyright RIME Developers
// Distributed under the BSD License
//
// register components
//
// 2011-06-30 GONG Chen <chen.sst@gmail.com>
//
#include <cstdint>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <string>
#include <system_error>
#ifdef _WIN32
#include <process.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif
#include <boost/interprocess/file_mapping.hpp>
#include <boost/interprocess/mapped_region.hpp>
#include <rime/dict/mapped_file.h>

namespace rime {

class MappedFileImpl {
 public:
  enum OpenMode {
    kOpenReadOnly,
    kOpenReadWrite,
  };

  MappedFileImpl(const path& file_path, OpenMode mode) {
    boost::interprocess::mode_t file_mapping_mode =
        (mode == kOpenReadOnly) ? boost::interprocess::read_only
                                : boost::interprocess::read_write;
    file_.reset(new boost::interprocess::file_mapping(file_path.c_str(),
                                                      file_mapping_mode));
    region_.reset(
        new boost::interprocess::mapped_region(*file_, file_mapping_mode));
  }
  ~MappedFileImpl() {
    region_.reset();
    file_.reset();
  }
  bool Flush() { return region_->flush(); }
  void* get_address() const { return region_->get_address(); }
  size_t get_size() const { return region_->get_size(); }

 private:
  the<boost::interprocess::file_mapping> file_;
  the<boost::interprocess::mapped_region> region_;
};

// 蓬萊：開檔、mmap 失敗（例如 0 位元組的檔不能 map）boost 會丟出例外，這裡接住、回 false，
// 不讓例外一路丟到建 session、部署（11/7：截成 0 位元組的 .table.bin 讓部署整個中止、修不好）
static bool map_file(the<MappedFileImpl>* file,
                     const path& file_path,
                     MappedFileImpl::OpenMode mode) {
  try {
    file->reset(new MappedFileImpl(file_path, mode));
  } catch (const std::exception& ex) {
    LOG(ERROR) << "error mapping file '" << file_path << "': " << ex.what();
    file->reset();
    return false;
  }
  return true;
}

// 蓬萊：暫存檔放在同一個資料夾（換名才是同一個檔案系統裡的原子操作），檔名帶程序編號，
// 兩個程序同時部署同一個資料夾時各寫各的，換名換上去的都是寫完的檔
static path temp_path_for(const path& file_path) {
#ifdef _WIN32
  int pid = _getpid();
#else
  int pid = getpid();
#endif
  path temp(file_path);
  temp += ".tmp." + std::to_string(pid);
  return temp;
}

static bool sync_file(const path& file_path) {
#ifdef _WIN32
  return true;
#else
  int fd = ::open(file_path.c_str(), O_RDWR);
  if (fd < 0)
    return false;
  bool ok = ::fsync(fd) == 0;
  ::close(fd);
  return ok;
#endif
}

MappedFile::MappedFile(const path& file_path) : file_path_(file_path) {}

MappedFile::~MappedFile() {
  Close();
  DiscardTemp();
}

// 蓬萊：寫到同一個資料夾的暫存檔（原本直接在正式路徑建檔，已經有的就原地 Resize），
// ShrinkToFit 寫完才換名換上去：舊檔的內容不動，別的程序 mmap 著舊檔照樣讀得到，寫到一半失敗正式的檔也不動
bool MappedFile::Create(size_t capacity) {
  Close();
  DiscardTemp();
  temp_path_ = temp_path_for(file_path_);
  LOG(INFO) << "creating file '" << temp_path_ << "' for '" << file_path_
            << "'.";
  {
    std::filebuf fbuf;
    if (!fbuf.open(temp_path_.c_str(), std::ios_base::in | std::ios_base::out |
                                           std::ios_base::trunc |
                                           std::ios_base::binary)) {
      LOG(ERROR) << "error creating file '" << temp_path_ << "'.";
      temp_path_.clear();
      return false;
    }
    if (capacity > 0) {
      fbuf.pubseekoff(capacity - 1, std::ios_base::beg);
      fbuf.sputc(0);
    }
    fbuf.close();
  }
  LOG(INFO) << "opening file for read/write access.";
  if (!map_file(&file_, temp_path_, MappedFileImpl::kOpenReadWrite)) {
    DiscardTemp();
    return false;
  }
  size_ = 0;
  return true;
}

bool MappedFile::OpenReadOnly() {
  DiscardTemp();  // 蓬萊：沒寫完的暫存檔不要了，讀正式的檔
  if (!Exists()) {
    LOG(ERROR) << "attempt to open non-existent file '" << file_path_ << "'.";
    return false;
  }
  if (!map_file(&file_, file_path_, MappedFileImpl::kOpenReadOnly)) {
    size_ = 0;
    return false;
  }
  size_ = file_->get_size();
  return true;
}

bool MappedFile::OpenReadWrite() {
  const path& file_path = active_path();
  if (!std::filesystem::exists(file_path)) {
    LOG(ERROR) << "attempt to open non-existent file '" << file_path << "'.";
    return false;
  }
  if (!map_file(&file_, file_path, MappedFileImpl::kOpenReadWrite))
    return false;
  size_ = 0;
  return true;
}

void MappedFile::Close() {
  if (file_) {
    file_.reset();
    size_ = 0;
  }
}

bool MappedFile::Exists() const {
  return std::filesystem::exists(file_path_);
}

bool MappedFile::IsOpen() const {
  return bool(file_);
}

bool MappedFile::Flush() {
  if (!file_)
    return false;
  return file_->Flush();
}

bool MappedFile::ShrinkToFit() {
  LOG(INFO) << "shrinking file to fit data size. capacity: " << capacity();
  return Resize(size_) && Commit();
}

// 蓬萊：暫存檔寫進磁碟（fsync）再換名成正式的檔；Create 之後才有暫存檔，沒有就什麼都不做
bool MappedFile::Commit() {
  if (temp_path_.empty())
    return true;
  if (!sync_file(temp_path_)) {
    LOG(ERROR) << "error syncing file '" << temp_path_ << "'.";
    DiscardTemp();
    return false;
  }
  std::error_code ec;
  std::filesystem::rename(temp_path_, file_path_, ec);
  if (ec) {
    LOG(ERROR) << "error renaming '" << temp_path_ << "' to '" << file_path_
               << "': " << ec.message();
    DiscardTemp();
    return false;
  }
  temp_path_.clear();
  return true;
}

void MappedFile::DiscardTemp() {
  if (temp_path_.empty())
    return;
  Close();
  std::error_code ec;
  std::filesystem::remove(temp_path_, ec);
  temp_path_.clear();
}

bool MappedFile::Remove() {
  if (IsOpen())
    Close();
  DiscardTemp();
  return boost::interprocess::file_mapping::remove(file_path_.c_str());
}

bool MappedFile::Resize(size_t capacity) {
  LOG(INFO) << "resize file to: " << capacity;
  if (IsOpen())
    Close();
  try {
    std::filesystem::resize_file(active_path(), capacity);
  } catch (...) {
    return false;
  }
  return true;
}

bool MappedFile::InBounds(const void* ptr,
                          size_t elem_size,
                          size_t count) const {
  if (!file_ || !ptr || elem_size == 0)
    return false;
  const uintptr_t begin = reinterpret_cast<uintptr_t>(address());
  const uintptr_t p = reinterpret_cast<uintptr_t>(ptr);
  if (p < begin || p - begin > size_)
    return false;
  return count <= (size_ - (p - begin)) / elem_size;
}

bool MappedFile::StringInBounds(const String& str) const {
  const char* p = str.c_str();
  if (!p)
    return true;
  if (!InBounds(p, 1, 1))
    return false;
  const size_t rest = size_ - (p - address());
  return std::memchr(p, 0, rest) != nullptr;
}

String* MappedFile::CreateString(const string& str) {
  String* ret = Allocate<String>();
  if (ret && !str.empty()) {
    CopyString(str, ret);
  }
  return ret;
}

bool MappedFile::CopyString(const string& src, String* dest) {
  if (!dest)
    return false;
  size_t size = src.length() + 1;
  char* ptr = Allocate<char>(size);
  if (!ptr)
    return false;
  std::strncpy(ptr, src.c_str(), size);
  dest->data = ptr;
  return true;
}

size_t MappedFile::capacity() const {
  return file_->get_size();
}

char* MappedFile::address() const {
  return reinterpret_cast<char*>(file_->get_address());
}

}  // namespace rime
