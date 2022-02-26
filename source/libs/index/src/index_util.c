/*
 * Copyright (c) 2019 TAOS Data, Inc. <jhtao@taosdata.com>
 *
 * This program is free software: you can use, redistribute, and/or modify
 * it under the terms of the GNU Affero General Public License, version 3
 * or later ("AGPL"), as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */
#include "index_util.h"
#include "index.h"
typedef struct MergeIndex {
  int idx;
  int len;
} MergeIndex;

static int iBinarySearch(SArray *arr, int s, int e, uint64_t k) {
  uint64_t v;
  int32_t  m;
  while (s <= e) {
    m = s + (e - s) / 2;
    v = *(uint64_t *)taosArrayGet(arr, m);
    if (v >= k) {
      e = m - 1;
    } else {
      s = m + 1;
    }
  }
  return s;
}

void iIntersection(SArray *inters, SArray *final) {
  int32_t sz = taosArrayGetSize(inters);
  if (sz <= 0) {
    return;
  }
  MergeIndex *mi = calloc(sz, sizeof(MergeIndex));
  for (int i = 0; i < sz; i++) {
    SArray *t = taosArrayGetP(inters, i);
    mi[i].len = taosArrayGetSize(t);
    mi[i].idx = 0;
  }

  SArray *base = taosArrayGetP(inters, 0);
  for (int i = 0; i < taosArrayGetSize(base); i++) {
    uint64_t tgt = *(uint64_t *)taosArrayGet(base, i);
    bool     has = true;
    for (int j = 1; j < taosArrayGetSize(inters); j++) {
      SArray *oth = taosArrayGetP(inters, j);
      int     mid = iBinarySearch(oth, mi[j].idx, mi[j].len - 1, tgt);
      if (mid >= 0 && mid < mi[j].len) {
        uint64_t val = *(uint64_t *)taosArrayGet(oth, mid);
        has = (val == tgt ? true : false);
        mi[j].idx = mid;
      } else {
        has = false;
      }
    }
    if (has == true) {
      taosArrayPush(final, &tgt);
    }
  }
  tfree(mi);
}
void iUnion(SArray *inters, SArray *final) {
  int32_t sz = taosArrayGetSize(inters);
  if (sz <= 0) {
    return;
  }
  MergeIndex *mi = calloc(sz, sizeof(MergeIndex));
  for (int i = 0; i < sz; i++) {
    SArray *t = taosArrayGetP(inters, i);
    mi[i].len = taosArrayGetSize(t);
    mi[i].idx = 0;
  }
  while (1) {
    uint64_t mVal = UINT_MAX;
    int      mIdx = -1;

    for (int j = 0; j < sz; j++) {
      SArray *t = taosArrayGetP(inters, j);
      if (mi[j].idx >= mi[j].len) {
        continue;
      }
      uint64_t cVal = *(uint64_t *)taosArrayGet(t, mi[j].idx);
      if (cVal < mVal) {
        mVal = cVal;
        mIdx = j;
      }
    }
    if (mIdx != -1) {
      mi[mIdx].idx++;
      if (taosArrayGetSize(final) > 0) {
        uint64_t lVal = *(uint64_t *)taosArrayGetLast(final);
        if (lVal == mVal) {
          continue;
        }
      }
      taosArrayPush(final, &mVal);
    } else {
      break;
    }
  }

  tfree(mi);
}