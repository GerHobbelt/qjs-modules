#define _GNU_SOURCE
#include "getdents.h"
#include "char-utils.h"
#include <assert.h>

#if defined(_WIN32) || defined(__MSYS__)
#include <windows.h>

#define BUFFER_SIZE 1024 * 1024 * 5
#define DIRENT(d) ((find_data_type*)&(d)->fdw)

#ifdef FIND_A
typedef WIN32_FIND_DATA find_data_type;
#else
typedef struct _wfinddata64_t find_data_type;
#endif

struct getdents_reader {
  union {
    HANDLE h_ptr;
    intptr_t h_int;
  };
  BOOL first;
  find_data_type fdw;
};

#ifndef FIND_A
#define cFileName name
#define dwFileAttributes attrib
#define FindNextFile _wfindnext64
#define h_ptr h_int
#endif

size_t
getdents_size() {
  return sizeof(struct getdents_reader);
}

void
getdents_clear(Directory* d) {
  d->h_ptr = INVALID_HANDLE_VALUE;
  d->first = FALSE;
}

intptr_t
getdents_handle(Directory* d) {
  return (intptr_t)d->h_ptr;
}

int
getdents_open(Directory* d, const char* path) {
  /*size_t pathlen = utf8_strlen(path, strlen(path));*/
#ifdef FIND_A
  if((d->h_ptr = FindFirstFile(path, &d->fdw)) != INVALID_HANDLE_VALUE)
    d->first = TRUE;
#else
  wchar_t* wp = utf8_towcs(path);
  assert(wp);

  if((HANDLE)(d->h_int = _wfindfirst64(wp, &d->fdw)) != INVALID_HANDLE_VALUE)
    d->first = TRUE;

  free(wp);
#endif
  return d->h_ptr == INVALID_HANDLE_VALUE ? -1 : 0;
}

int
getdents_adopt(Directory* d, intptr_t hnd) {
  if((HANDLE)hnd == INVALID_HANDLE_VALUE)
    return -1;
  d->h_int = hnd;
  return 0;
}

DirEntry*
getdents_read(Directory* d) {
  if(d->first)
    d->first = FALSE;
  else if(!FindNextFile(d->h_ptr, &d->fdw))
    return 0;

  return (DirEntry*)&d->fdw;
}

const void*
getdents_name(const DirEntry* e) {
  find_data_type* fdw = (void*)e;

  return fdw->cFileName;
}

const uint8_t*
getdents_namebuf(const DirEntry* e, size_t* len) {
  const wchar_t* s = ((find_data_type*)e)->cFileName;

  if(len)
    *len = wcslen(s);
  return (const uint8_t*)s;
}

void
getdents_close(Directory* d) {
  CloseHandle(d->h_ptr);
  d->h_ptr = INVALID_HANDLE_VALUE;
}

int
getdents_isblk(const DirEntry* e) {
  return !!(((find_data_type*)e)->dwFileAttributes & FILE_ATTRIBUTE_DEVICE);
}

int
getdents_ischr(const DirEntry* e) {
  return !!(((find_data_type*)e)->dwFileAttributes & FILE_ATTRIBUTE_DEVICE);
}

int
getdents_isdir(const DirEntry* e) {
  return !!(((find_data_type*)e)->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
}

int
getdents_isfifo(const DirEntry* e) {
  return !!(((find_data_type*)e)->dwFileAttributes == FILE_ATTRIBUTE_DIRECTORY);
}

int
getdents_islnk(const DirEntry* e) {
  return ((find_data_type*)e)->dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT;
}

int
getdents_isreg(const DirEntry* e) {
  return !!(((find_data_type*)e)->dwFileAttributes & FILE_ATTRIBUTE_NORMAL);
}

int
getdents_issock(const DirEntry* e) {
  return 0;
}

#else
#include <dirent.h> /* Defines DT_* constants */
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/syscall.h>

#define BUFFER_SIZE 1024 * 1024 * 5
#define DIRENT(d) ((struct linux_dirent64*)&(d)->buf[(d)->bpos])

struct linux_dirent {
  long d_ino;
  off_t d_off;
  unsigned short d_reclen;
  char d_name[];
};

struct linux_dirent64 {
  ino64_t d_ino;           /* 64-bit inode number */
  off64_t d_off;           /* 64-bit offset to next structure */
  unsigned short d_reclen; /* Size of this dirent */
  unsigned char d_type;    /* File type */
  char d_name[];           /* Filename (null-terminated) */
};

struct getdents_reader {
  int fd, nread, bpos;
  char buf[BUFFER_SIZE];
};

size_t
getdents_size() {
  return sizeof(Directory);
}

void
getdents_clear(Directory* d) {
  d->fd = -1;
  d->nread = d->bpos = 0;
}

intptr_t
getdents_handle(Directory* d) {
  return d->fd;
}

int
getdents_open(Directory* d, const char* path) {
  d->nread = d->bpos = 0;

  if((d->fd = open(path, O_RDONLY | O_DIRECTORY)) == -1)
    return -1;

  return 0;
}

int
getdents_adopt(Directory* d, intptr_t fd) {
  struct stat st;
  d->nread = d->bpos = 0;
  if(fstat(fd, &st) == -1)
    return -1;

  d->fd = fd;
  return 0;
}

DirEntry*
getdents_read(Directory* d) {
  for(;;) {
    if(!d->nread || d->bpos >= d->nread) {
      d->bpos = 0;
      d->nread = syscall(SYS_getdents64, d->fd, d->buf, sizeof(d->buf));
      if(d->nread <= 0)
        break;
    }
    while(d->bpos < d->nread) {
      struct linux_dirent64* e = DIRENT(d);
      char d_type = d->buf[d->bpos + e->d_reclen - 1];
      d->bpos += e->d_reclen;

      if(e->d_ino != 0 /*&& d_type == DT_REG*/)
        return (DirEntry*)e;
    }
  }

  return 0;
}

const void*
getdents_name(const DirEntry* e) {
  return ((struct linux_dirent64*)e)->d_name;
}

const uint8_t*
getdents_namebuf(const DirEntry* e, size_t* len) {
  const char* name = ((struct linux_dirent64*)e)->d_name;
  if(len)
    *len = strlen(name);
  return (const uint8_t*)name;
}

void
getdents_close(Directory* d) {
  close(d->fd);
  d->fd = -1;
}

int
getdents_isblk(const DirEntry* e) {
  return ((struct linux_dirent64*)e)->d_type == DT_BLK;
}

int
getdents_ischr(const DirEntry* e) {
  return ((struct linux_dirent64*)e)->d_type == DT_CHR;
}

int
getdents_isdir(const DirEntry* e) {
  return ((struct linux_dirent64*)e)->d_type == DT_DIR;
}

int
getdents_isfifo(const DirEntry* e) {
  return ((struct linux_dirent64*)e)->d_type == DT_FIFO;
}

int
getdents_islnk(const DirEntry* e) {
  return ((struct linux_dirent64*)e)->d_type == DT_LNK;
}

int
getdents_isreg(const DirEntry* e) {
  return ((struct linux_dirent64*)e)->d_type == DT_REG;
}

int
getdents_issock(const DirEntry* e) {
  return ((struct linux_dirent64*)e)->d_type == DT_SOCK;
}

#endif /* defined(_WIN32) */

int
getdents_type(const DirEntry* e) {
  if(getdents_isblk(e))
    return TYPE_BLK;
  if(getdents_ischr(e))
    return TYPE_CHR;
  if(getdents_isdir(e))
    return TYPE_DIR;
  if(getdents_isfifo(e))
    return TYPE_FIFO;
  if(getdents_islnk(e))
    return TYPE_LNK;
  if(getdents_isreg(e))
    return TYPE_REG;
  if(getdents_issock(e))
    return TYPE_SOCK;
  return 0;
}
