


#include <iostream>

#include <string.h>
#include <stdlib.h>
#include <errno.h>

#include "include/types.h"
#include "include/utime.h"
#include "objclass/objclass.h"

CLS_VER(1,0)
CLS_NAME(rgw)

cls_handle_t h_class;
cls_method_handle_t h_rgw_bucket_list;
cls_method_handle_t h_rgw_bucket_modify;

struct rgw_bucket_dir_entry {
  std::string name;
  uint64_t size;
  utime_t mtime;
  uint64_t epoch;

  void encode(bufferlist &bl) const {
    __u8 struct_v = 1;
    ::encode(struct_v, bl);
    ::encode(name, bl);
    ::encode(mtime, bl);
    ::encode(epoch, bl);
  }
  void decode(bufferlist::iterator &bl) {
    __u8 struct_v;
    ::decode(struct_v, bl);
    ::decode(name, bl);
    ::decode(mtime, bl);
    ::decode(epoch, bl);
  }
};
WRITE_CLASS_ENCODER(rgw_bucket_dir_entry)

struct rgw_bucket_dir_header {
  uint64_t total_size;
  uint64_t num_entries;

  void encode(bufferlist &bl) const {
    __u8 struct_v = 1;
    ::encode(struct_v, bl);
    ::encode(total_size, bl);
    ::encode(num_entries, bl);
  }
  void decode(bufferlist::iterator &bl) {
    __u8 struct_v;
    ::decode(struct_v, bl);
    ::decode(total_size, bl);
    ::decode(num_entries, bl);
  }
};
WRITE_CLASS_ENCODER(rgw_bucket_dir_header)

struct rgw_bucket_dir {
  struct rgw_bucket_dir_header header;
  std::map<string, struct rgw_bucket_dir_entry> m;

  void encode(bufferlist &bl) const {
    __u8 struct_v = 1;
    ::encode(struct_v, bl);
    ::encode(header, bl);
    ::encode(m, bl);
  }
  void decode(bufferlist::iterator &bl) {
    __u8 struct_v;
    ::decode(struct_v, bl);
    ::decode(header, bl);
    ::decode(m, bl);
  }
};
WRITE_CLASS_ENCODER(rgw_bucket_dir)

enum modify_op {
  CLS_RGW_OP_ADD = 0,
  CLS_RGW_OP_DEL = 1,
};

static int read_bucket_dir(cls_method_context_t hctx, struct rgw_bucket_dir& dir)
{
  bufferlist bl;
  bufferlist::iterator iter;

  int rc = cls_cxx_read(hctx, 0, 0, &bl);
  if (rc < 0)
    return rc;

  try {
    bufferlist::iterator iter = bl.begin();
    ::decode(dir, iter);
  } catch (buffer::error& err) {
    CLS_LOG("ERROR: rgw_bucket_list(): failed to decode buffer\n");
    return -EIO;
  }

  return 0;
}

static int write_bucket_dir(cls_method_context_t hctx, struct rgw_bucket_dir& dir)
{
  bufferlist bl;

  ::encode(dir, bl);

  int rc = cls_cxx_write_full(hctx, &bl);
  return rc;
}

int rgw_bucket_list(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  bufferlist bl;
  struct rgw_bucket_dir dir;
  int rc = read_bucket_dir(hctx, dir);
  if (rc < 0)
    return rc;

  bufferlist::iterator iter = in->begin();
  string start_obj;
  uint32_t num_entries;

  try {
    ::decode(start_obj, iter);
    ::decode(num_entries, iter);
  } catch (buffer::error& err) {
    CLS_LOG("ERROR: rgw_bucket_list(): failed to decode request\n");
    return -EINVAL;
  }

  struct rgw_bucket_dir new_dir;
  new_dir.header = dir.header;
  std::map<string, struct rgw_bucket_dir_entry>& m = new_dir.m;
  std::map<string, struct rgw_bucket_dir_entry>::iterator miter;
  uint32_t i;
  for (i = 0, miter = dir.m.begin(); i !=num_entries && miter != dir.m.end(); ++i, ++miter) {
    m[miter->first] = miter->second;
  }
  ::encode(new_dir, *out);

  return 0;
}

int rgw_bucket_modify(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  bufferlist bl;
  struct rgw_bucket_dir dir;
  int rc = read_bucket_dir(hctx, dir);
  if (rc < 0)
    return rc;

  uint8_t op;
  uint64_t epoch;
  struct rgw_bucket_dir_entry entry;

  bufferlist::iterator iter = in->begin();
  try {
    ::decode(op, iter);
    ::decode(epoch, iter);
    ::decode(entry, iter);
  } catch (buffer::error& err) {
    CLS_LOG("ERROR: rgw_bucket_modify(): failed to decode request\n");
    return -EINVAL;
  }
  CLS_LOG("rgw_bucket_modify(): request: op=%d name=%s epoch=%lld\n", op, entry.name.c_str(), epoch);

  std::map<string, struct rgw_bucket_dir_entry>::iterator miter = dir.m.find(entry.name);

  if (miter != dir.m.end()) {
    CLS_LOG("rgw_bucket_modify(): existing entry: epoch=%lld\n", entry.epoch);
    if (entry.epoch >= epoch) {
      CLS_LOG("rgw_bucket_modify(): skipping request, old epoch\n");
      return 0;
    }
  }

  switch (op) {
  case CLS_RGW_OP_DEL:
    if (miter != dir.m.end())
      dir.m.erase(miter);
    else
      return -ENOENT;
    break;
  case CLS_RGW_OP_ADD:
    dir.m[entry.name] = entry;
    break;
  }

  rc = write_bucket_dir(hctx, dir);

  return rc;
}

void __cls_init()
{
  CLS_LOG("Loaded rgw class!");

  cls_register("rgw", &h_class);
  cls_register_cxx_method(h_class, "rgw_bucket_list", CLS_METHOD_RD | CLS_METHOD_PUBLIC, rgw_bucket_list, &h_rgw_bucket_list);
  cls_register_cxx_method(h_class, "rgw_bucket_modify", CLS_METHOD_RD | CLS_METHOD_WR | CLS_METHOD_PUBLIC, rgw_bucket_modify, &h_rgw_bucket_modify);

  return;
}

