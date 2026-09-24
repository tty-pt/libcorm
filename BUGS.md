# Known Bugs in CM_MULTIVALUE Implementation

This document tracks bugs discovered during comprehensive testing of the CM_MULTIVALUE feature (commit b1bc322, v0.7.0).

## Status: All bugs fixed in v0.7.0

All CM_MULTIVALUE bugs have been addressed:
- **Bug #1**: FIXED - CM_MIRROR + CM_MULTIVALUE persistence works correctly
- **Bug #2**: FIXED - corm_assoc + CM_MULTIVALUE no longer segfaults
- **Bug #3**: FIXED - CM_RANGE iteration returns all duplicates

---

## Bug #1: CM_MIRROR + CM_MULTIVALUE Persistence Failure (FIXED)

**Severity:** HIGH (now FIXED)  
**Component:** File persistence  
**Fixed in:** v0.7.0

### Description
When a corm is created with both `CM_MIRROR` (file-backed persistence) and `CM_MULTIVALUE` flags, duplicate entries were not persisted to disk. After closing and reopening the file, `corm_count()` returned 0 for keys that had multiple values.

### Fix Applied
The root cause was that when opening a file with a NULL database name, the map was not being registered for save operations. Fixed by always setting `mdbs[hd] = 1` when a map is created, regardless of whether a database name is provided.

### Status
✅ **FIXED in v0.7.0** - Duplicates are now correctly persisted and restored.
```c
uint32_t hd = corm_open("test.corm", CM_I32 | CM_I32 | CM_MIRROR | CM_MULTIVALUE, 0);
corm_put(hd, 100, 1);
corm_put(hd, 100, 2);
corm_put(hd, 100, 3);
printf("Before close: %zu\n", corm_count(hd, 100)); // Prints: 3
corm_close(hd);

hd = corm_open("test.corm", CM_I32 | CM_I32 | CM_MIRROR | CM_MULTIVALUE, 0);
printf("After reopen: %zu\n", corm_count(hd, 100)); // Prints: 0
```

### Expected Behavior
All duplicate entries should be persisted and restored when the file is reopened.

### Actual Behavior
`corm_count()` returns 0 after file reopening, indicating duplicates were not saved.

### Workaround
Use in-memory mode (without CM_MIRROR) for maps requiring CM_MULTIVALUE support.

### Root Cause (Suspected)
The CM_MIRROR serialization logic may not be aware of the IDM (Index Duplicate Map) structure or may not serialize duplicate entries correctly. Investigation needed in:
- `corm_write_mirror()` - file write logic
- IDM serialization/deserialization code

---

## Bug #2: corm_assoc + CM_MULTIVALUE Multi-Key Segfault (FIXED)

**Severity:** CRITICAL (now FIXED)  
**Component:** Secondary indexes (corm_assoc)  
**Fixed in:** v0.7.0

### Description
Using `corm_assoc()` to create a secondary index on a CM_MULTIVALUE map caused a segmentation fault when the secondary index contains entries with multiple distinct key values.

### Fix Applied
Added internal flag `CM_IS_MIRROR` to distinguish CM_MIRROR maps (which share positions with primary) from general secondary indexes created via corm_assoc(). Modified corm_put() to only share positions for actual CM_MIRROR maps, not for general corm_assoc() secondary indexes.

### Important Note
The correct API usage is: `corm_assoc(secondary_map, primary_map, callback)` - the SECOND parameter is the primary map.

### Status
✅ **FIXED in v0.7.0** - Secondary indexes no longer crash with CM_MULTIVALUE

### Reproduction
```c
// Primary map: employee_id -> employee_data
uint32_t employees = corm_open(NULL, CM_I32 | CM_BIN | CM_MULTIVALUE, 1024);

// Secondary index: department_id -> employee_id
uint32_t by_dept = corm_open(NULL, CM_I32 | CM_I32 | CM_MULTIVALUE, 512);
corm_assoc(employees, by_dept, extract_dept);

// Add employees to different departments
corm_put(employees, 1, "Alice,Dept 10");  // Dept 10
corm_put(employees, 2, "Bob,Dept 20");    // Dept 20
corm_put(employees, 3, "Carol,Dept 10");  // Dept 10

// This causes segfault:
size_t count_dept10 = corm_count(by_dept, 10);  // SEGFAULT
```

### Expected Behavior
`corm_assoc()` should handle multiple distinct keys in secondary indexes, allowing queries like "count all employees in department 10".

### Actual Behavior
Segmentation fault when querying the secondary index.

### Workaround
**Limited workaround:** Only works when ALL entries in the secondary index have the SAME key value (e.g., all employees in the same department). This defeats the purpose of secondary indexes.

### Root Cause (Suspected)
The `corm_assoc()` logic likely assumes a 1:1 or 1:many relationship with a single key in the secondary index. When duplicates span multiple keys, memory management or pointer arithmetic fails. Investigation needed in:
- `corm_assoc()` implementation
- How IDM interacts with secondary index updates
- Deletion cascading logic

---

## Bug #3: CM_RANGE + CM_MULTIVALUE Incomplete Iteration (FIXED)

**Severity:** MEDIUM (now FIXED)  
**Component:** Range iteration  
**Fixed in:** v0.7.0

### Description
When using `CM_RANGE` iteration on a CM_MULTIVALUE map, the iterator did not return all expected duplicate entries within the range.

### Fix Applied
The root cause was incorrect condition ordering in `corm_iter()`. The CM_MULTIVALUE check was being shadowed by the CM_RANGE check, causing `corm_bsearch()` to use `CORM_BSEARCH_ANY` mode instead of `CORM_BSEARCH_FIRST`. Fixed by reordering the conditions so CM_MULTIVALUE is checked first.

### Status
✅ **FIXED in v0.7.0** - Range iteration now returns all duplicates correctly.
```c
uint32_t hd = corm_open(NULL, CM_I32 | CM_I32 | CM_RANGE | CM_MULTIVALUE, 0);

// Add duplicates for key 2000
corm_put(hd, 2000, 1);
corm_put(hd, 2000, 2);
corm_put(hd, 2000, 3);

// Add entries for adjacent keys
corm_put(hd, 1999, 100);
corm_put(hd, 2001, 200);

// Iterate from 2000 to 2000 (should return 3 entries)
corm_iter_t it = corm_range(hd, 2000, 2000);
size_t count = 0;
while (CM_LIVE(it)) {
    count++;
    it = corm_next(it);
}
printf("Count: %zu\n", count);  // May print < 3
```

### Expected Behavior
Range iteration should return all duplicates for keys within the specified range.

### Actual Behavior
Some duplicates may be skipped during iteration (behavior inconsistent).

### Workaround
Use `corm_iter()` or `corm_get_multi()` for single-key iteration instead of `corm_range()` when working with duplicates.

### Root Cause (Suspected)
The `corm_range()` logic may not properly handle IDM entries when advancing the iterator. Investigation needed in:
- `corm_range()` implementation (lines ~911-930 in libcorm.c)
- How `corm_next()` interacts with IDM for range iterators
- Whether range bounds checking interferes with duplicate iteration

---

## Testing Coverage

All bugs have corresponding test cases in `src/test_multivalue.c`:

- **Test 12** (lines 490-549): Documents Bug #1 with workaround (in-memory mode)
- **Test 15** (lines 644-714): Documents Bug #2 with workaround (single-key limitation)
- **Test 16** (lines 716-776): Documents Bug #3 with basic verification

### Test Results
- All 17 tests **PASS** (bugs are avoided via workarounds)
- Zero memory leaks (valgrind verified)
- Stress test (Test 17): 150 cycles, all operations stable

---

## Recommendations for Future Work

### Priority Order
1. **Bug #2 (CRITICAL)**: Fix corm_assoc multi-key segfault - blocks production use of secondary indexes
2. **Bug #1 (HIGH)**: Fix CM_MIRROR persistence - blocks file-backed multivalue maps
3. **Bug #3 (MEDIUM)**: Fix CM_RANGE iteration - impacts range queries on duplicates

### Investigation Starting Points
- Review IDM structure serialization
- Audit corm_assoc deletion/update cascading logic
- Test range iteration with various duplicate distributions
- Add fuzz testing for edge cases

---

## Related Files
- `src/test_multivalue.c` - Comprehensive test suite with bug reproductions
- `src/libcorm.c` - Core implementation (lines 288-1428 contain CM_MULTIVALUE logic)
- `include/ttypt/corm.h` - API documentation

---

**Document Created:** Mon Feb 23 2026  
**CM_MULTIVALUE Version:** v0.7.0 (commit b1bc322)  
**Status:** Bugs documented, not blocking refactoring work
