/* -*- Mode: C; tab-width: 8; c-basic-offset: 2; indent-tabs-mode: nil; -*- */

#include "rrutil.h"

#define DUMMY_FILE "dummy.txt"

static void check_mapping(int* rpage, int* wpage, ssize_t nr_ints) {
  int i;
  for (i = 0; i < nr_ints; ++i) {
    test_assert(wpage[i] == rpage[i]);

    wpage[i] = i;

    test_assert(rpage[i] == i && wpage[i] == rpage[i]);
  }
  atomic_printf("  %p and %p point at the same resource\n", rpage, wpage);
}

static void overwrite_file(const char* path, ssize_t num_bytes) {
  const int magic = 0x5a5a5a5a;
  int fd = open(path, O_TRUNC | O_RDWR, 0600);
  size_t i;
  for (i = 0; i < num_bytes / sizeof(magic); ++i) {
    write(fd, &magic, sizeof(magic));
  }
  close(fd);
}

int main(void) {
  size_t num_bytes = sysconf(_SC_PAGESIZE);
  char file_name[] = "/tmp/rr-test-mremap-XXXXXX";
  int fd = mkstemp(file_name);
  int* wpage;
  int* rpage;
  int* old_wpage;

  test_assert(fd >= 0);

  overwrite_file(file_name, 2 * num_bytes);

  wpage = mmap(NULL, num_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  rpage = mmap(NULL, num_bytes, PROT_READ, MAP_SHARED, fd, 0);
  atomic_printf("wpage:%p rpage:%p\n", wpage, rpage);
  test_assert(wpage != (void*)-1 && rpage != (void*)-1 && rpage != wpage);

  check_mapping(rpage, wpage, num_bytes / sizeof(*wpage));

  overwrite_file(file_name, 2 * num_bytes);

  old_wpage = wpage;
  wpage = mremap(old_wpage, num_bytes, 2 * num_bytes, MREMAP_MAYMOVE);
  atomic_printf("remapped wpage:%p\n", wpage);
  test_assert(wpage != (void*)-1 && wpage != old_wpage);

  check_mapping(rpage, wpage, num_bytes / sizeof(*wpage));

  atomic_puts("EXIT-SUCCESS");

  return 0;
}
