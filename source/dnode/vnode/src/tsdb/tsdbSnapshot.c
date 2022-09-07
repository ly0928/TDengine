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

#include "tsdb.h"

// STsdbSnapReader ========================================
typedef enum { SNAP_DATA_FILE_ITER = 0, SNAP_STT_FILE_ITER } EFIterT;
typedef struct {
  SRBTreeNode n;
  SRowInfo    rInfo;
  EFIterT     type;
  union {
    struct {
      SArray*    aBlockIdx;
      int32_t    iBlockIdx;
      SBlockIdx* pBlockIdx;
      SMapData   mBlock;
      int32_t    iBlock;
    };  // .data file
    struct {
      int32_t iStt;
      SArray* aSttBlk;
      int32_t iSttBlk;
    };  // .stt file
  };
  SBlockData bData;
  int32_t    iRow;
} SFDataIter;

struct STsdbSnapReader {
  STsdb*  pTsdb;
  int64_t sver;
  int64_t ever;
  STsdbFS fs;
  int8_t  type;
  // for data file
  int8_t        dataDone;
  int32_t       fid;
  SDataFReader* pDataFReader;
  SFDataIter*   pIter;
  SRBTree       rbt;
  SFDataIter    aFDataIter[TSDB_MAX_STT_FILE + 1];
  SBlockData    bData;
  SSkmInfo      skmTable;
  // for del file
  int8_t       delDone;
  SDelFReader* pDelFReader;
  SArray*      aDelIdx;  // SArray<SDelIdx>
  int32_t      iDelIdx;
  SArray*      aDelData;  // SArray<SDelData>
  uint8_t*     aBuf[5];
};

extern int32_t tRowInfoCmprFn(const void* p1, const void* p2);
extern int32_t tsdbReadDataBlockEx(SDataFReader* pReader, SDataBlk* pDataBlk, SBlockData* pBlockData);
extern int32_t tsdbUpdateTableSchema(SMeta* pMeta, int64_t suid, int64_t uid, SSkmInfo* pSkmInfo);

static int32_t tsdbSnapReadOpenFile(STsdbSnapReader* pReader) {
  int32_t code = 0;

  SDFileSet  dFileSet = {.fid = pReader->fid};
  SDFileSet* pSet = taosArraySearch(pReader->fs.aDFileSet, &dFileSet, tDFileSetCmprFn, TD_GT);
  if (pSet == NULL) return code;

  pReader->fid = pSet->fid;
  code = tsdbDataFReaderOpen(&pReader->pDataFReader, pReader->pTsdb, pSet);
  if (code) goto _err;

  pReader->pIter = NULL;
  tRBTreeCreate(&pReader->rbt, tRowInfoCmprFn);

  // .data file
  SFDataIter* pIter = &pReader->aFDataIter[0];
  pIter->type = SNAP_DATA_FILE_ITER;

  code = tsdbReadBlockIdx(pReader->pDataFReader, pIter->aBlockIdx);
  if (code) goto _err;

  for (pIter->iBlockIdx = 0; pIter->iBlockIdx < taosArrayGetSize(pIter->aBlockIdx); pIter->iBlockIdx++) {
    pIter->pBlockIdx = (SBlockIdx*)taosArrayGet(pIter->aBlockIdx, pIter->iBlockIdx);

    code = tsdbReadBlock(pReader->pDataFReader, pIter->pBlockIdx, &pIter->mBlock);
    if (code) goto _err;

    for (pIter->iBlock = 0; pIter->iBlock < pIter->mBlock.nItem; pIter->iBlock++) {
      SDataBlk dataBlk;
      tMapDataGetItemByIdx(&pIter->mBlock, pIter->iBlock, &dataBlk, tGetDataBlk);

      if (dataBlk.minVer > pReader->ever || dataBlk.maxVer < pReader->sver) continue;

      code = tsdbReadDataBlockEx(pReader->pDataFReader, &dataBlk, &pIter->bData);
      if (code) goto _err;

      ASSERT(pIter->pBlockIdx->suid == pIter->bData.suid);
      ASSERT(pIter->pBlockIdx->uid == pIter->bData.uid);

      for (pIter->iRow = 0; pIter->iRow < pIter->bData.nRow; pIter->iRow++) {
        int64_t rowVer = pIter->bData.aVersion[pIter->iRow];

        if (rowVer >= pReader->sver && rowVer <= pReader->ever) {
          pIter->rInfo.suid = pIter->pBlockIdx->suid;
          pIter->rInfo.uid = pIter->pBlockIdx->uid;
          pIter->rInfo.row = tsdbRowFromBlockData(&pIter->bData, pIter->iRow);
          goto _add_iter_and_break;
        }
      }
    }

    continue;

  _add_iter_and_break:
    tRBTreePut(&pReader->rbt, (SRBTreeNode*)pIter);
    break;
  }

  // .stt file
  pIter = &pReader->aFDataIter[1];
  for (int32_t iStt = 0; iStt < pSet->nSttF; iStt++) {
    pIter->type = SNAP_STT_FILE_ITER;
    pIter->iStt = iStt;

    code = tsdbReadSttBlk(pReader->pDataFReader, iStt, pIter->aSttBlk);
    if (code) goto _err;

    for (pIter->iSttBlk = 0; pIter->iSttBlk < taosArrayGetSize(pIter->aSttBlk); pIter->iSttBlk++) {
      SSttBlk* pSttBlk = (SSttBlk*)taosArrayGet(pIter->aSttBlk, pIter->iSttBlk);

      if (pSttBlk->minVer > pReader->ever) continue;
      if (pSttBlk->maxVer < pReader->sver) continue;

      code = tsdbReadSttBlock(pReader->pDataFReader, iStt, pSttBlk, &pIter->bData);
      if (code) goto _err;

      for (pIter->iRow = 0; pIter->iRow < pIter->bData.nRow; pIter->iRow++) {
        int64_t rowVer = pIter->bData.aVersion[pIter->iRow];

        if (rowVer >= pReader->sver && rowVer <= pReader->ever) {
          pIter->rInfo.suid = pIter->bData.suid;
          pIter->rInfo.uid = pIter->bData.uid;
          pIter->rInfo.row = tsdbRowFromBlockData(&pIter->bData, pIter->iRow);
          goto _add_iter;
        }
      }
    }

    continue;

  _add_iter:
    tRBTreePut(&pReader->rbt, (SRBTreeNode*)pIter);
    pIter++;
  }

  tsdbInfo("vgId:%d, vnode snapshot tsdb open data file to read for %s, fid:%d", TD_VID(pReader->pTsdb->pVnode),
           pReader->pTsdb->path, pReader->fid);
  return code;

_err:
  tsdbError("vgId:%d vnode snapshot tsdb snap read open file failed since %s", TD_VID(pReader->pTsdb->pVnode),
            tstrerror(code));
  return code;
}

static SRowInfo* tsdbSnapGetRow(STsdbSnapReader* pReader) { return pReader->pIter ? &pReader->pIter->rInfo : NULL; }

static int32_t tsdbSnapNextRow(STsdbSnapReader* pReader) {
  int32_t code = 0;

  if (pReader->pIter) {
    SFDataIter* pIter = pReader->pIter;

    while (true) {
    _find_row:
      for (pIter->iRow++; pIter->iRow < pIter->bData.nRow; pIter->iRow++) {
        int64_t rowVer = pIter->bData.aVersion[pIter->iRow];

        if (rowVer >= pReader->sver && rowVer <= pReader->ever) {
          pIter->rInfo.uid = pIter->bData.uid ? pIter->bData.uid : pIter->bData.aUid[pIter->iRow];
          pIter->rInfo.row = tsdbRowFromBlockData(&pIter->bData, pIter->iRow);
          goto _out;
        }
      }

      if (pIter->type == SNAP_DATA_FILE_ITER) {
        while (true) {
          for (pIter->iBlock++; pIter->iBlock < pIter->mBlock.nItem; pIter->iBlock++) {
            SDataBlk dataBlk;
            tMapDataGetItemByIdx(&pIter->mBlock, pIter->iBlock, &dataBlk, tGetDataBlk);

            if (dataBlk.minVer > pReader->ever || dataBlk.maxVer < pReader->sver) continue;

            code = tsdbReadDataBlockEx(pReader->pDataFReader, &dataBlk, &pIter->bData);
            if (code) goto _err;

            pIter->iRow = -1;
            goto _find_row;
          }

          pIter->iBlockIdx++;
          if (pIter->iBlockIdx >= taosArrayGetSize(pIter->aBlockIdx)) break;

          pIter->pBlockIdx = (SBlockIdx*)taosArrayGet(pIter->aBlockIdx, pIter->iBlockIdx);
          code = tsdbReadBlock(pReader->pDataFReader, pIter->pBlockIdx, &pIter->mBlock);
          if (code) goto _err;
          pIter->iBlock = -1;
        }

        pReader->pIter = NULL;
      } else if (pIter->type == SNAP_STT_FILE_ITER) {
        for (pIter->iSttBlk++; pIter->iSttBlk < taosArrayGetSize(pIter->aSttBlk); pIter->iSttBlk++) {
          SSttBlk* pSttBlk = (SSttBlk*)taosArrayGet(pIter->aSttBlk, pIter->iSttBlk);

          if (pSttBlk->minVer > pReader->ever || pSttBlk->maxVer < pReader->sver) continue;

          code = tsdbReadSttBlock(pReader->pDataFReader, pIter->iStt, pSttBlk, &pIter->bData);
          if (code) goto _err;

          pIter->iRow = -1;
          goto _find_row;
        }

        pReader->pIter = NULL;
      } else {
        ASSERT(0);
      }
    }

  _out:
    pIter = (SFDataIter*)tRBTreeMin(&pReader->rbt);
    if (pReader->pIter && pIter) {
      int32_t c = tRowInfoCmprFn(&pReader->pIter->rInfo, &pIter->rInfo);
      if (c > 0) {
        tRBTreePut(&pReader->rbt, (SRBTreeNode*)pReader->pIter);
        pReader->pIter = NULL;
      } else {
        ASSERT(c);
      }
    }
  }

  if (pReader->pIter == NULL) {
    pReader->pIter = (SFDataIter*)tRBTreeMin(&pReader->rbt);
    if (pReader->pIter) {
      tRBTreeDrop(&pReader->rbt, (SRBTreeNode*)pReader->pIter);
    }
  }

  return code;

_err:
  return code;
}

static int32_t tsdbSnapCmprData(STsdbSnapReader* pReader, uint8_t** ppData) {
  int32_t code = 0;

  ASSERT(pReader->bData.nRow);

  int32_t aBufN[5] = {0};
  code = tCmprBlockData(&pReader->bData, TWO_STAGE_COMP, NULL, NULL, pReader->aBuf, aBufN);
  if (code) goto _exit;

  int32_t size = aBufN[0] + aBufN[1] + aBufN[2] + aBufN[3];
  *ppData = taosMemoryMalloc(sizeof(SSnapDataHdr) + size);
  if (*ppData == NULL) {
    code = TSDB_CODE_OUT_OF_MEMORY;
    goto _exit;
  }

  SSnapDataHdr* pHdr = (SSnapDataHdr*)*ppData;
  pHdr->type = SNAP_DATA_TSDB;
  pHdr->size = size;

  memcpy(pHdr->data, pReader->aBuf[3], aBufN[3]);
  memcpy(pHdr->data + aBufN[3], pReader->aBuf[2], aBufN[2]);
  if (aBufN[1]) {
    memcpy(pHdr->data + aBufN[3] + aBufN[2], pReader->aBuf[1], aBufN[1]);
  }
  if (aBufN[0]) {
    memcpy(pHdr->data + aBufN[3] + aBufN[2] + aBufN[1], pReader->aBuf[0], aBufN[0]);
  }

_exit:
  return code;
}

static int32_t tsdbSnapReadData(STsdbSnapReader* pReader, uint8_t** ppData) {
  int32_t code = 0;
  STsdb*  pTsdb = pReader->pTsdb;

  while (true) {
    if (pReader->pDataFReader == NULL) {
      code = tsdbSnapReadOpenFile(pReader);
      if (code) goto _err;
    }

    if (pReader->pDataFReader == NULL) break;

    SRowInfo* pRowInfo = tsdbSnapGetRow(pReader);
    if (pRowInfo == NULL) {
      tsdbDataFReaderClose(&pReader->pDataFReader);
      continue;
    }

    TABLEID     id = {.suid = pRowInfo->suid, .uid = pRowInfo->uid};
    SBlockData* pBlockData = &pReader->bData;

    code = tsdbUpdateTableSchema(pTsdb->pVnode->pMeta, id.suid, id.uid, &pReader->skmTable);
    if (code) goto _err;

    code = tBlockDataInit(pBlockData, id.suid, id.uid, pReader->skmTable.pTSchema);
    if (code) goto _err;

    while (pRowInfo->suid == id.suid && pRowInfo->uid == id.uid) {
      code = tBlockDataAppendRow(pBlockData, &pRowInfo->row, NULL, pRowInfo->uid);
      if (code) goto _err;

      code = tsdbSnapNextRow(pReader);
      if (code) goto _err;

      pRowInfo = tsdbSnapGetRow(pReader);
      if (pRowInfo == NULL) {
        tsdbDataFReaderClose(&pReader->pDataFReader);
        break;
      }

      if (pBlockData->nRow >= 4096) break;
    }

    code = tsdbSnapCmprData(pReader, ppData);
    if (code) goto _err;

    break;
  }

  return code;

_err:
  tsdbError("vgId:%d, vnode snapshot tsdb read data for %s failed since %s", TD_VID(pTsdb->pVnode), pTsdb->path,
            tstrerror(code));
  return code;
}

static int32_t tsdbSnapReadDel(STsdbSnapReader* pReader, uint8_t** ppData) {
  int32_t   code = 0;
  STsdb*    pTsdb = pReader->pTsdb;
  SDelFile* pDelFile = pReader->fs.pDelFile;

  if (pReader->pDelFReader == NULL) {
    if (pDelFile == NULL) {
      goto _exit;
    }

    // open
    code = tsdbDelFReaderOpen(&pReader->pDelFReader, pDelFile, pTsdb);
    if (code) goto _err;

    // read index
    code = tsdbReadDelIdx(pReader->pDelFReader, pReader->aDelIdx);
    if (code) goto _err;

    pReader->iDelIdx = 0;
  }

  while (true) {
    if (pReader->iDelIdx >= taosArrayGetSize(pReader->aDelIdx)) {
      tsdbDelFReaderClose(&pReader->pDelFReader);
      break;
    }

    SDelIdx* pDelIdx = (SDelIdx*)taosArrayGet(pReader->aDelIdx, pReader->iDelIdx);

    pReader->iDelIdx++;

    code = tsdbReadDelData(pReader->pDelFReader, pDelIdx, pReader->aDelData);
    if (code) goto _err;

    int32_t size = 0;
    for (int32_t iDelData = 0; iDelData < taosArrayGetSize(pReader->aDelData); iDelData++) {
      SDelData* pDelData = (SDelData*)taosArrayGet(pReader->aDelData, iDelData);

      if (pDelData->version >= pReader->sver && pDelData->version <= pReader->ever) {
        size += tPutDelData(NULL, pDelData);
      }
    }
    if (size == 0) continue;

    // org data
    size = sizeof(TABLEID) + size;
    *ppData = taosMemoryMalloc(sizeof(SSnapDataHdr) + size);
    if (*ppData == NULL) {
      code = TSDB_CODE_OUT_OF_MEMORY;
      goto _err;
    }

    SSnapDataHdr* pHdr = (SSnapDataHdr*)(*ppData);
    pHdr->type = SNAP_DATA_DEL;
    pHdr->size = size;

    TABLEID* pId = (TABLEID*)(&pHdr[1]);
    pId->suid = pDelIdx->suid;
    pId->uid = pDelIdx->uid;
    int32_t n = sizeof(SSnapDataHdr) + sizeof(TABLEID);
    for (int32_t iDelData = 0; iDelData < taosArrayGetSize(pReader->aDelData); iDelData++) {
      SDelData* pDelData = (SDelData*)taosArrayGet(pReader->aDelData, iDelData);

      if (pDelData->version < pReader->sver) continue;
      if (pDelData->version > pReader->ever) continue;

      n += tPutDelData((*ppData) + n, pDelData);
    }

    tsdbInfo("vgId:%d, vnode snapshot tsdb read del data for %s, suid:%" PRId64 " uid:%d" PRId64 " size:%d",
             TD_VID(pTsdb->pVnode), pTsdb->path, pDelIdx->suid, pDelIdx->uid, size);

    break;
  }

_exit:
  return code;

_err:
  tsdbError("vgId:%d, vnode snapshot tsdb read del for %s failed since %s", TD_VID(pTsdb->pVnode), pTsdb->pVnode,
            tstrerror(code));
  return code;
}

int32_t tsdbSnapReaderOpen(STsdb* pTsdb, int64_t sver, int64_t ever, int8_t type, STsdbSnapReader** ppReader) {
  int32_t          code = 0;
  STsdbSnapReader* pReader = NULL;

  // alloc
  pReader = (STsdbSnapReader*)taosMemoryCalloc(1, sizeof(*pReader));
  if (pReader == NULL) {
    code = TSDB_CODE_OUT_OF_MEMORY;
    goto _err;
  }
  pReader->pTsdb = pTsdb;
  pReader->sver = sver;
  pReader->ever = ever;
  pReader->type = type;

  code = taosThreadRwlockRdlock(&pTsdb->rwLock);
  if (code) {
    code = TAOS_SYSTEM_ERROR(code);
    goto _err;
  }

  code = tsdbFSRef(pTsdb, &pReader->fs);
  if (code) {
    taosThreadRwlockUnlock(&pTsdb->rwLock);
    goto _err;
  }

  code = taosThreadRwlockUnlock(&pTsdb->rwLock);
  if (code) {
    code = TAOS_SYSTEM_ERROR(code);
    goto _err;
  }

  // data
  pReader->fid = INT32_MIN;
  for (int32_t iIter = 0; iIter < sizeof(pReader->aFDataIter) / sizeof(pReader->aFDataIter[0]); iIter++) {
    SFDataIter* pIter = &pReader->aFDataIter[iIter];

    if (iIter == 0) {
      pIter->aBlockIdx = taosArrayInit(0, sizeof(SBlockIdx));
      if (pIter->aBlockIdx == NULL) {
        code = TSDB_CODE_OUT_OF_MEMORY;
        goto _err;
      }
    } else {
      pIter->aSttBlk = taosArrayInit(0, sizeof(SSttBlk));
      if (pIter->aSttBlk == NULL) {
        code = TSDB_CODE_OUT_OF_MEMORY;
        goto _err;
      }
    }

    code = tBlockDataCreate(&pIter->bData);
    if (code) goto _err;
  }

  code = tBlockDataCreate(&pReader->bData);
  if (code) goto _err;

  // del
  pReader->aDelIdx = taosArrayInit(0, sizeof(SDelIdx));
  if (pReader->aDelIdx == NULL) {
    code = TSDB_CODE_OUT_OF_MEMORY;
    goto _err;
  }
  pReader->aDelData = taosArrayInit(0, sizeof(SDelData));
  if (pReader->aDelData == NULL) {
    code = TSDB_CODE_OUT_OF_MEMORY;
    goto _err;
  }

  tsdbInfo("vgId:%d, vnode snapshot tsdb reader opened for %s", TD_VID(pTsdb->pVnode), pTsdb->path);
  *ppReader = pReader;
  return code;

_err:
  tsdbError("vgId:%d, vnode snapshot tsdb reader open for %s failed since %s", TD_VID(pTsdb->pVnode), pTsdb->path,
            tstrerror(code));
  *ppReader = NULL;
  return code;
}

int32_t tsdbSnapReaderClose(STsdbSnapReader** ppReader) {
  int32_t          code = 0;
  STsdbSnapReader* pReader = *ppReader;

  // data
  if (pReader->pDataFReader) tsdbDataFReaderClose(&pReader->pDataFReader);
  for (int32_t iIter = 0; iIter < sizeof(pReader->aFDataIter) / sizeof(pReader->aFDataIter[0]); iIter++) {
    SFDataIter* pIter = &pReader->aFDataIter[iIter];

    if (iIter == 0) {
      taosArrayDestroy(pIter->aBlockIdx);
      tMapDataClear(&pIter->mBlock);
    } else {
      taosArrayDestroy(pIter->aSttBlk);
    }

    tBlockDataDestroy(&pIter->bData, 1);
  }

  tBlockDataDestroy(&pReader->bData, 1);
  tTSchemaDestroy(pReader->skmTable.pTSchema);

  // del
  if (pReader->pDelFReader) tsdbDelFReaderClose(&pReader->pDelFReader);
  taosArrayDestroy(pReader->aDelIdx);
  taosArrayDestroy(pReader->aDelData);

  tsdbFSUnref(pReader->pTsdb, &pReader->fs);

  tsdbInfo("vgId:%d, vnode snapshot tsdb reader closed for %s", TD_VID(pReader->pTsdb->pVnode), pReader->pTsdb->path);

  for (int32_t iBuf = 0; iBuf < sizeof(pReader->aBuf) / sizeof(pReader->aBuf[0]); iBuf++) {
    tFree(pReader->aBuf[iBuf]);
  }

  taosMemoryFree(pReader);
  *ppReader = NULL;
  return code;
}

int32_t tsdbSnapRead(STsdbSnapReader* pReader, uint8_t** ppData) {
  int32_t code = 0;

  *ppData = NULL;

  // read data file
  if (!pReader->dataDone) {
    code = tsdbSnapReadData(pReader, ppData);
    if (code) {
      goto _err;
    } else {
      if (*ppData) {
        goto _exit;
      } else {
        pReader->dataDone = 1;
      }
    }
  }

  // read del file
  if (!pReader->delDone) {
    code = tsdbSnapReadDel(pReader, ppData);
    if (code) {
      goto _err;
    } else {
      if (*ppData) {
        goto _exit;
      } else {
        pReader->delDone = 1;
      }
    }
  }

_exit:
  tsdbDebug("vgId:%d, vnode snapshot tsdb read for %s", TD_VID(pReader->pTsdb->pVnode), pReader->pTsdb->path);
  return code;

_err:
  tsdbError("vgId:%d, vnode snapshot tsdb read for %s failed since %s", TD_VID(pReader->pTsdb->pVnode),
            pReader->pTsdb->path, tstrerror(code));
  return code;
}

// STsdbSnapWriter ========================================
struct STsdbSnapWriter {
  STsdb*  pTsdb;
  int64_t sver;
  int64_t ever;
  STsdbFS fs;

  // config
  int32_t minutes;
  int8_t  precision;
  int32_t minRow;
  int32_t maxRow;
  int8_t  cmprAlg;
  int64_t commitID;

  uint8_t* aBuf[5];
  // for data file
  SBlockData bData;

  int32_t       fid;
  SDataFReader* pDataFReader;
  SArray*       aBlockIdx;  // SArray<SBlockIdx>
  int32_t       iBlockIdx;
  SBlockIdx*    pBlockIdx;
  SMapData      mBlock;  // SMapData<SDataBlk>
  int32_t       iBlock;
  SBlockData*   pBlockData;
  int32_t       iRow;
  SBlockData    bDataR;
  SArray*       aSstBlk;  // SArray<SSttBlk>
  int32_t       iBlockL;
  SBlockData    lDataR;

  SDataFWriter* pDataFWriter;
  SBlockIdx*    pBlockIdxW;  // NULL when no committing table
  SDataBlk      blockW;
  SBlockData    bDataW;
  SBlockIdx     blockIdxW;

  SMapData mBlockW;     // SMapData<SDataBlk>
  SArray*  aBlockIdxW;  // SArray<SBlockIdx>
  SArray*  aBlockLW;    // SArray<SSttBlk>

  // for del file
  SDelFReader* pDelFReader;
  SDelFWriter* pDelFWriter;
  int32_t      iDelIdx;
  SArray*      aDelIdxR;
  SArray*      aDelData;
  SArray*      aDelIdxW;
};

static int32_t tsdbSnapWriteTableDataEnd(STsdbSnapWriter* pWriter) {
  int32_t code = 0;

  ASSERT(pWriter->pDataFWriter);

  if (pWriter->pBlockIdxW == NULL) goto _exit;

  // consume remain rows
  if (pWriter->pBlockData) {
    ASSERT(pWriter->iRow < pWriter->pBlockData->nRow);
    while (pWriter->iRow < pWriter->pBlockData->nRow) {
      code = tBlockDataAppendRow(&pWriter->bDataW, &tsdbRowFromBlockData(pWriter->pBlockData, pWriter->iRow), NULL,
                                 0);  // todo
      if (code) goto _err;

      if (pWriter->bDataW.nRow >= pWriter->maxRow * 4 / 5) {
        // pWriter->blockW.last = 0;
        // code = tsdbWriteBlockData(pWriter->pDataFWriter, &pWriter->bDataW, NULL, NULL, pWriter->pBlockIdxW,
        //                           &pWriter->blockW, pWriter->cmprAlg);
        if (code) goto _err;

        code = tMapDataPutItem(&pWriter->mBlockW, &pWriter->blockW, tPutDataBlk);
        if (code) goto _err;

        tDataBlkReset(&pWriter->blockW);
        tBlockDataClear(&pWriter->bDataW);
      }

      pWriter->iRow++;
    }
  }

  // write remain data if has
  if (pWriter->bDataW.nRow > 0) {
    // pWriter->blockW.last = 0;
    if (pWriter->bDataW.nRow < pWriter->minRow) {
      if (pWriter->iBlock > pWriter->mBlock.nItem) {
        // pWriter->blockW.last = 1;
      }
    }

    // code = tsdbWriteBlockData(pWriter->pDataFWriter, &pWriter->bDataW, NULL, NULL, pWriter->pBlockIdxW,
    //                           &pWriter->blockW, pWriter->cmprAlg);
    // if (code) goto _err;

    code = tMapDataPutItem(&pWriter->mBlockW, &pWriter->blockW, tPutDataBlk);
    if (code) goto _err;
  }

  while (true) {
    if (pWriter->iBlock >= pWriter->mBlock.nItem) break;

    SDataBlk block;
    tMapDataGetItemByIdx(&pWriter->mBlock, pWriter->iBlock, &block, tGetDataBlk);

    // if (block.last) {
    //   code = tsdbReadBlockData(pWriter->pDataFReader, pWriter->pBlockIdx, &block, &pWriter->bDataR, NULL, NULL);
    //   if (code) goto _err;

    //   tBlockReset(&block);
    //   block.last = 1;
    //   code = tsdbWriteBlockData(pWriter->pDataFWriter, &pWriter->bDataR, NULL, NULL, pWriter->pBlockIdxW, &block,
    //                             pWriter->cmprAlg);
    //   if (code) goto _err;
    // }

    code = tMapDataPutItem(&pWriter->mBlockW, &block, tPutDataBlk);
    if (code) goto _err;

    pWriter->iBlock++;
  }

  // SDataBlk
  // code = tsdbWriteBlock(pWriter->pDataFWriter, &pWriter->mBlockW, NULL, pWriter->pBlockIdxW);
  // if (code) goto _err;

  // SBlockIdx
  if (taosArrayPush(pWriter->aBlockIdxW, pWriter->pBlockIdxW) == NULL) {
    code = TSDB_CODE_OUT_OF_MEMORY;
    goto _err;
  }

_exit:
  tsdbInfo("vgId:%d, tsdb snapshot write table data end for %s", TD_VID(pWriter->pTsdb->pVnode), pWriter->pTsdb->path);
  return code;

_err:
  tsdbError("vgId:%d, tsdb snapshot write table data end for %s failed since %s", TD_VID(pWriter->pTsdb->pVnode),
            pWriter->pTsdb->path, tstrerror(code));
  return code;
}

static int32_t tsdbSnapMoveWriteTableData(STsdbSnapWriter* pWriter, SBlockIdx* pBlockIdx) {
  int32_t code = 0;

  code = tsdbReadBlock(pWriter->pDataFReader, pBlockIdx, &pWriter->mBlock);
  if (code) goto _err;

  // SBlockData
  SDataBlk block;
  tMapDataReset(&pWriter->mBlockW);
  for (int32_t iBlock = 0; iBlock < pWriter->mBlock.nItem; iBlock++) {
    tMapDataGetItemByIdx(&pWriter->mBlock, iBlock, &block, tGetDataBlk);

    // if (block.last) {
    //   code = tsdbReadBlockData(pWriter->pDataFReader, pBlockIdx, &block, &pWriter->bDataR, NULL, NULL);
    //   if (code) goto _err;

    //   tBlockReset(&block);
    //   block.last = 1;
    //   code =
    //       tsdbWriteBlockData(pWriter->pDataFWriter, &pWriter->bDataR, NULL, NULL, pBlockIdx, &block,
    //       pWriter->cmprAlg);
    //   if (code) goto _err;
    // }

    code = tMapDataPutItem(&pWriter->mBlockW, &block, tPutDataBlk);
    if (code) goto _err;
  }

  // SDataBlk
  SBlockIdx blockIdx = {.suid = pBlockIdx->suid, .uid = pBlockIdx->uid};
  code = tsdbWriteBlock(pWriter->pDataFWriter, &pWriter->mBlockW, &blockIdx);
  if (code) goto _err;

  // SBlockIdx
  if (taosArrayPush(pWriter->aBlockIdxW, &blockIdx) == NULL) {
    code = TSDB_CODE_OUT_OF_MEMORY;
    goto _err;
  }

_exit:
  return code;

_err:
  tsdbError("vgId:%d, tsdb snapshot move write table data for %s failed since %s", TD_VID(pWriter->pTsdb->pVnode),
            pWriter->pTsdb->path, tstrerror(code));
  return code;
}

static int32_t tsdbSnapWriteTableDataImpl(STsdbSnapWriter* pWriter) {
  int32_t     code = 0;
  SBlockData* pBlockData = &pWriter->bData;
  int32_t     iRow = 0;
  TSDBROW     row;
  TSDBROW*    pRow = &row;

  // // correct schema
  // code = tBlockDataCorrectSchema(&pWriter->bDataW, pBlockData);
  // if (code) goto _err;

  // loop to merge
  *pRow = tsdbRowFromBlockData(pBlockData, iRow);
  while (true) {
    if (pRow == NULL) break;

    if (pWriter->pBlockData) {
      ASSERT(pWriter->iRow < pWriter->pBlockData->nRow);

      int32_t c = tsdbRowCmprFn(pRow, &tsdbRowFromBlockData(pWriter->pBlockData, pWriter->iRow));

      ASSERT(c);

      if (c < 0) {
        // code = tBlockDataAppendRow(&pWriter->bDataW, pRow, NULL);
        // if (code) goto _err;

        iRow++;
        if (iRow < pWriter->pBlockData->nRow) {
          *pRow = tsdbRowFromBlockData(pBlockData, iRow);
        } else {
          pRow = NULL;
        }
      } else if (c > 0) {
        // code = tBlockDataAppendRow(&pWriter->bDataW, &tsdbRowFromBlockData(pWriter->pBlockData, pWriter->iRow),
        // NULL); if (code) goto _err;

        pWriter->iRow++;
        if (pWriter->iRow >= pWriter->pBlockData->nRow) {
          pWriter->pBlockData = NULL;
        }
      }
    } else {
      TSDBKEY key = TSDBROW_KEY(pRow);

      while (true) {
        if (pWriter->iBlock >= pWriter->mBlock.nItem) break;

        SDataBlk block;
        int32_t  c;

        tMapDataGetItemByIdx(&pWriter->mBlock, pWriter->iBlock, &block, tGetDataBlk);

        // if (block.last) {
        //   pWriter->pBlockData = &pWriter->bDataR;

        //   code = tsdbReadBlockData(pWriter->pDataFReader, pWriter->pBlockIdx, &block, pWriter->pBlockData, NULL,
        //   NULL); if (code) goto _err; pWriter->iRow = 0;

        //   pWriter->iBlock++;
        //   break;
        // }

        c = tsdbKeyCmprFn(&block.maxKey, &key);

        ASSERT(c);

        if (c < 0) {
          if (pWriter->bDataW.nRow) {
            // pWriter->blockW.last = 0;
            // code = tsdbWriteBlockData(pWriter->pDataFWriter, &pWriter->bDataW, NULL, NULL, pWriter->pBlockIdxW,
            //                           &pWriter->blockW, pWriter->cmprAlg);
            // if (code) goto _err;

            code = tMapDataPutItem(&pWriter->mBlockW, &pWriter->blockW, tPutDataBlk);
            if (code) goto _err;

            tDataBlkReset(&pWriter->blockW);
            tBlockDataClear(&pWriter->bDataW);
          }

          code = tMapDataPutItem(&pWriter->mBlockW, &block, tPutDataBlk);
          if (code) goto _err;

          pWriter->iBlock++;
        } else {
          c = tsdbKeyCmprFn(&tBlockDataLastKey(pBlockData), &block.minKey);

          ASSERT(c);

          if (c > 0) {
            pWriter->pBlockData = &pWriter->bDataR;
            // code =
            //     tsdbReadBlockData(pWriter->pDataFReader, pWriter->pBlockIdx, &block, pWriter->pBlockData, NULL,
            //     NULL);
            // if (code) goto _err;
            pWriter->iRow = 0;

            pWriter->iBlock++;
          }
          break;
        }
      }

      if (pWriter->pBlockData) continue;

      // code = tBlockDataAppendRow(&pWriter->bDataW, pRow, NULL);
      // if (code) goto _err;

      iRow++;
      if (iRow < pBlockData->nRow) {
        *pRow = tsdbRowFromBlockData(pBlockData, iRow);
      } else {
        pRow = NULL;
      }
    }

  _check_write:
    if (pWriter->bDataW.nRow < pWriter->maxRow * 4 / 5) continue;

  _write_block:
    // code = tsdbWriteBlockData(pWriter->pDataFWriter, &pWriter->bDataW, NULL, NULL, pWriter->pBlockIdxW,
    //                           &pWriter->blockW, pWriter->cmprAlg);
    // if (code) goto _err;

    code = tMapDataPutItem(&pWriter->mBlockW, &pWriter->blockW, tPutDataBlk);
    if (code) goto _err;

    tDataBlkReset(&pWriter->blockW);
    tBlockDataClear(&pWriter->bDataW);
  }

  return code;

_err:
  tsdbError("vgId:%d, vnode snapshot tsdb write table data impl for %s failed since %s", TD_VID(pWriter->pTsdb->pVnode),
            pWriter->pTsdb->path, tstrerror(code));
  return code;
}

static int32_t tsdbSnapWriteTableData(STsdbSnapWriter* pWriter, TABLEID id) {
  int32_t     code = 0;
  SBlockData* pBlockData = &pWriter->bData;
  TSDBKEY     keyFirst = tBlockDataFirstKey(pBlockData);
  TSDBKEY     keyLast = tBlockDataLastKey(pBlockData);

  // end last table write if should
  if (pWriter->pBlockIdxW) {
    int32_t c = tTABLEIDCmprFn(pWriter->pBlockIdxW, &id);
    if (c < 0) {
      // end
      code = tsdbSnapWriteTableDataEnd(pWriter);
      if (code) goto _err;

      // reset
      pWriter->pBlockIdxW = NULL;
    } else if (c > 0) {
      ASSERT(0);
    }
  }

  // start new table data write if need
  if (pWriter->pBlockIdxW == NULL) {
    // write table data ahead
    while (true) {
      if (pWriter->iBlockIdx >= taosArrayGetSize(pWriter->aBlockIdx)) break;

      SBlockIdx* pBlockIdx = (SBlockIdx*)taosArrayGet(pWriter->aBlockIdx, pWriter->iBlockIdx);
      int32_t    c = tTABLEIDCmprFn(pBlockIdx, &id);

      if (c >= 0) break;

      code = tsdbSnapMoveWriteTableData(pWriter, pBlockIdx);
      if (code) goto _err;

      pWriter->iBlockIdx++;
    }

    // reader
    pWriter->pBlockIdx = NULL;
    if (pWriter->iBlockIdx < taosArrayGetSize(pWriter->aBlockIdx)) {
      ASSERT(pWriter->pDataFReader);

      SBlockIdx* pBlockIdx = (SBlockIdx*)taosArrayGet(pWriter->aBlockIdx, pWriter->iBlockIdx);
      int32_t    c = tTABLEIDCmprFn(pBlockIdx, &id);

      ASSERT(c >= 0);

      if (c == 0) {
        pWriter->pBlockIdx = pBlockIdx;
        pWriter->iBlockIdx++;
      }
    }

    if (pWriter->pBlockIdx) {
      code = tsdbReadBlock(pWriter->pDataFReader, pWriter->pBlockIdx, &pWriter->mBlock);
      if (code) goto _err;
    } else {
      tMapDataReset(&pWriter->mBlock);
    }
    pWriter->iBlock = 0;
    pWriter->pBlockData = NULL;
    pWriter->iRow = 0;

    // writer
    pWriter->pBlockIdxW = &pWriter->blockIdxW;
    pWriter->pBlockIdxW->suid = id.suid;
    pWriter->pBlockIdxW->uid = id.uid;

    tDataBlkReset(&pWriter->blockW);
    tBlockDataReset(&pWriter->bDataW);
    tMapDataReset(&pWriter->mBlockW);
  }

  ASSERT(pWriter->pBlockIdxW && pWriter->pBlockIdxW->suid == id.suid && pWriter->pBlockIdxW->uid == id.uid);
  ASSERT(pWriter->pBlockIdx == NULL || (pWriter->pBlockIdx->suid == id.suid && pWriter->pBlockIdx->uid == id.uid));

  code = tsdbSnapWriteTableDataImpl(pWriter);
  if (code) goto _err;

_exit:
  tsdbDebug("vgId:%d, vnode snapshot tsdb write data impl for %s", TD_VID(pWriter->pTsdb->pVnode),
            pWriter->pTsdb->path);
  return code;

_err:
  tsdbError("vgId:%d, vnode snapshot tsdb write data impl for %s failed since %s", TD_VID(pWriter->pTsdb->pVnode),
            pWriter->pTsdb->path, tstrerror(code));
  return code;
}

static int32_t tsdbSnapWriteDataEnd(STsdbSnapWriter* pWriter) {
  int32_t code = 0;
  STsdb*  pTsdb = pWriter->pTsdb;

  if (pWriter->pDataFWriter == NULL) goto _exit;

  // finish current table
  code = tsdbSnapWriteTableDataEnd(pWriter);
  if (code) goto _err;

  // move remain table
  while (pWriter->iBlockIdx < taosArrayGetSize(pWriter->aBlockIdx)) {
    code = tsdbSnapMoveWriteTableData(pWriter, (SBlockIdx*)taosArrayGet(pWriter->aBlockIdx, pWriter->iBlockIdx));
    if (code) goto _err;

    pWriter->iBlockIdx++;
  }

  // write remain stuff
  if (taosArrayGetSize(pWriter->aBlockLW) > 0) {
    code = tsdbWriteSttBlk(pWriter->pDataFWriter, pWriter->aBlockIdxW);
    if (code) goto _err;
  }

  if (taosArrayGetSize(pWriter->aBlockIdx) > 0) {
    code = tsdbWriteBlockIdx(pWriter->pDataFWriter, pWriter->aBlockIdxW);
    if (code) goto _err;
  }

  code = tsdbFSUpsertFSet(&pWriter->fs, &pWriter->pDataFWriter->wSet);
  if (code) goto _err;

  code = tsdbDataFWriterClose(&pWriter->pDataFWriter, 1);
  if (code) goto _err;

  if (pWriter->pDataFReader) {
    code = tsdbDataFReaderClose(&pWriter->pDataFReader);
    if (code) goto _err;
  }

_exit:
  tsdbInfo("vgId:%d, vnode snapshot tsdb writer data end for %s", TD_VID(pTsdb->pVnode), pTsdb->path);
  return code;

_err:
  tsdbError("vgId:%d, vnode snapshot tsdb writer data end for %s failed since %s", TD_VID(pTsdb->pVnode), pTsdb->path,
            tstrerror(code));
  return code;
}

static int32_t tsdbSnapWriteData(STsdbSnapWriter* pWriter, uint8_t* pData, uint32_t nData) {
  int32_t       code = 0;
  STsdb*        pTsdb = pWriter->pTsdb;
  SSnapDataHdr* pHdr = (SSnapDataHdr*)pData;
  TABLEID       id = *(TABLEID*)(pData + sizeof(SSnapDataHdr));
  int64_t       n;

  // decode
  SBlockData* pBlockData = &pWriter->bData;
  code = tDecmprBlockData(pData + sizeof(SSnapDataHdr) + sizeof(TABLEID), pHdr->size - sizeof(TABLEID), pBlockData,
                          pWriter->aBuf);
  if (code) goto _err;

  // open file
  TSDBKEY keyFirst = {.version = pBlockData->aVersion[0], .ts = pBlockData->aTSKEY[0]};
  TSDBKEY keyLast = {.version = pBlockData->aVersion[pBlockData->nRow - 1],
                     .ts = pBlockData->aTSKEY[pBlockData->nRow - 1]};

  int32_t fid = tsdbKeyFid(keyFirst.ts, pWriter->minutes, pWriter->precision);
  ASSERT(fid == tsdbKeyFid(keyLast.ts, pWriter->minutes, pWriter->precision));
  if (pWriter->pDataFWriter == NULL || pWriter->fid != fid) {
    // end last file data write if need
    code = tsdbSnapWriteDataEnd(pWriter);
    if (code) goto _err;

    pWriter->fid = fid;

    // read
    SDFileSet* pSet = taosArraySearch(pWriter->fs.aDFileSet, &(SDFileSet){.fid = fid}, tDFileSetCmprFn, TD_EQ);
    if (pSet) {
      code = tsdbDataFReaderOpen(&pWriter->pDataFReader, pTsdb, pSet);
      if (code) goto _err;

      code = tsdbReadBlockIdx(pWriter->pDataFReader, pWriter->aBlockIdx);
      if (code) goto _err;

      code = tsdbReadSttBlk(pWriter->pDataFReader, 0, pWriter->aSstBlk);
      if (code) goto _err;
    } else {
      ASSERT(pWriter->pDataFReader == NULL);
      taosArrayClear(pWriter->aBlockIdx);
      taosArrayClear(pWriter->aSstBlk);
    }
    pWriter->iBlockIdx = 0;
    pWriter->pBlockIdx = NULL;
    tMapDataReset(&pWriter->mBlock);
    pWriter->iBlock = 0;
    pWriter->pBlockData = NULL;
    pWriter->iRow = 0;
    pWriter->iBlockL = 0;
    tBlockDataReset(&pWriter->bDataR);
    tBlockDataReset(&pWriter->lDataR);

    // write
    SHeadFile fHead;
    SDataFile fData;
    SSttFile  fLast;
    SSmaFile  fSma;
    SDFileSet wSet = {.pHeadF = &fHead, .pDataF = &fData, .aSttF[0] = &fLast, .pSmaF = &fSma};

    if (pSet) {
      wSet.diskId = pSet->diskId;
      wSet.fid = fid;
      wSet.nSttF = 1;
      fHead = (SHeadFile){.commitID = pWriter->commitID, .offset = 0, .size = 0};
      fData = *pSet->pDataF;
      fLast = (SSttFile){.commitID = pWriter->commitID, .size = 0};
      fSma = *pSet->pSmaF;
    } else {
      wSet.diskId = (SDiskID){.level = 0, .id = 0};
      wSet.fid = fid;
      wSet.nSttF = 1;
      fHead = (SHeadFile){.commitID = pWriter->commitID, .offset = 0, .size = 0};
      fData = (SDataFile){.commitID = pWriter->commitID, .size = 0};
      fLast = (SSttFile){.commitID = pWriter->commitID, .size = 0, .offset = 0};
      fSma = (SSmaFile){.commitID = pWriter->commitID, .size = 0};
    }

    code = tsdbDataFWriterOpen(&pWriter->pDataFWriter, pTsdb, &wSet);
    if (code) goto _err;

    taosArrayClear(pWriter->aBlockIdxW);
    taosArrayClear(pWriter->aBlockLW);
    tMapDataReset(&pWriter->mBlockW);
    pWriter->pBlockIdxW = NULL;
    tBlockDataReset(&pWriter->bDataW);
  }

  code = tsdbSnapWriteTableData(pWriter, id);
  if (code) goto _err;

  tsdbInfo("vgId:%d, vnode snapshot tsdb write data for %s, fid:%d suid:%" PRId64 " uid:%" PRId64 " nRow:%d",
           TD_VID(pTsdb->pVnode), pTsdb->path, fid, id.suid, id.suid, pBlockData->nRow);
  return code;

_err:
  tsdbError("vgId:%d, vnode snapshot tsdb write data for %s failed since %s", TD_VID(pTsdb->pVnode), pTsdb->path,
            tstrerror(code));
  return code;
}

static int32_t tsdbSnapMoveWriteDelData(STsdbSnapWriter* pWriter, TABLEID* pId) {
  int32_t code = 0;

  while (true) {
    if (pWriter->iDelIdx >= taosArrayGetSize(pWriter->aDelIdxR)) break;

    SDelIdx* pDelIdx = (SDelIdx*)taosArrayGet(pWriter->aDelIdxR, pWriter->iDelIdx);

    if (tTABLEIDCmprFn(pDelIdx, pId) >= 0) break;

    code = tsdbReadDelData(pWriter->pDelFReader, pDelIdx, pWriter->aDelData);
    if (code) goto _exit;

    SDelIdx delIdx = *pDelIdx;
    code = tsdbWriteDelData(pWriter->pDelFWriter, pWriter->aDelData, &delIdx);
    if (code) goto _exit;

    if (taosArrayPush(pWriter->aDelIdxW, &delIdx) == NULL) {
      code = TSDB_CODE_OUT_OF_MEMORY;
      goto _exit;
    }

    pWriter->iDelIdx++;
  }

_exit:
  return code;
}

static int32_t tsdbSnapWriteDel(STsdbSnapWriter* pWriter, uint8_t* pData, uint32_t nData) {
  int32_t code = 0;
  STsdb*  pTsdb = pWriter->pTsdb;

  // Open del file if not opened yet
  if (pWriter->pDelFWriter == NULL) {
    SDelFile* pDelFile = pWriter->fs.pDelFile;

    // reader
    if (pDelFile) {
      code = tsdbDelFReaderOpen(&pWriter->pDelFReader, pDelFile, pTsdb);
      if (code) goto _err;

      code = tsdbReadDelIdx(pWriter->pDelFReader, pWriter->aDelIdxR);
      if (code) goto _err;
    } else {
      taosArrayClear(pWriter->aDelIdxR);
    }
    pWriter->iDelIdx = 0;

    // writer
    SDelFile delFile = {.commitID = pWriter->commitID};
    code = tsdbDelFWriterOpen(&pWriter->pDelFWriter, &delFile, pTsdb);
    if (code) goto _err;
    taosArrayClear(pWriter->aDelIdxW);
  }

  SSnapDataHdr* pHdr = (SSnapDataHdr*)pData;
  TABLEID       id = *(TABLEID*)pHdr->data;

  ASSERT(pHdr->size + sizeof(SSnapDataHdr) == nData);

  // Move write data < id
  code = tsdbSnapMoveWriteDelData(pWriter, &id);
  if (code) goto _err;

  // Merge incoming data with current
  if (pWriter->iDelIdx < taosArrayGetSize(pWriter->aDelIdxR) &&
      tTABLEIDCmprFn(taosArrayGet(pWriter->aDelIdxR, pWriter->iDelIdx), &id) == 0) {
    SDelIdx* pDelIdx = (SDelIdx*)taosArrayGet(pWriter->aDelIdxR, pWriter->iDelIdx);

    code = tsdbReadDelData(pWriter->pDelFReader, pDelIdx, pWriter->aDelData);
    if (code) goto _err;

    pWriter->iDelIdx++;
  } else {
    taosArrayClear(pWriter->aDelData);
  }

  int64_t n = sizeof(SSnapDataHdr) + sizeof(TABLEID);
  while (n < nData) {
    SDelData delData;

    n += tGetDelData(pData + n, &delData);

    if (taosArrayPush(pWriter->aDelData, &delData) == NULL) {
      code = TSDB_CODE_OUT_OF_MEMORY;
      goto _err;
    }
  }

  SDelIdx delIdx = {.suid = id.suid, .uid = id.uid};
  code = tsdbWriteDelData(pWriter->pDelFWriter, pWriter->aDelData, &delIdx);
  if (code) goto _err;

  if (taosArrayPush(pWriter->aDelIdxW, &delIdx) == NULL) {
    code = TSDB_CODE_OUT_OF_MEMORY;
    goto _err;
  }

_exit:
  return code;

_err:
  tsdbError("vgId:%d, vnode snapshot tsdb write del for %s failed since %s", TD_VID(pTsdb->pVnode), pTsdb->path,
            tstrerror(code));
  return code;
}

static int32_t tsdbSnapWriteDelEnd(STsdbSnapWriter* pWriter) {
  int32_t code = 0;
  STsdb*  pTsdb = pWriter->pTsdb;

  if (pWriter->pDelFWriter == NULL) goto _exit;

  TABLEID id = {.suid = INT64_MAX, .uid = INT64_MAX};
  code = tsdbSnapMoveWriteDelData(pWriter, &id);
  if (code) goto _err;

  code = tsdbUpdateDelFileHdr(pWriter->pDelFWriter);
  if (code) goto _err;

  code = tsdbFSUpsertDelFile(&pWriter->fs, &pWriter->pDelFWriter->fDel);
  if (code) goto _err;

  code = tsdbDelFWriterClose(&pWriter->pDelFWriter, 1);
  if (code) goto _err;

  if (pWriter->pDelFReader) {
    code = tsdbDelFReaderClose(&pWriter->pDelFReader);
    if (code) goto _err;
  }

_exit:
  tsdbInfo("vgId:%d, vnode snapshot tsdb write del for %s end", TD_VID(pTsdb->pVnode), pTsdb->path);
  return code;

_err:
  tsdbError("vgId:%d, vnode snapshot tsdb write del end for %s failed since %s", TD_VID(pTsdb->pVnode), pTsdb->path,
            tstrerror(code));
  return code;
}

int32_t tsdbSnapWriterOpen(STsdb* pTsdb, int64_t sver, int64_t ever, STsdbSnapWriter** ppWriter) {
  int32_t          code = 0;
  STsdbSnapWriter* pWriter = NULL;

  // alloc
  pWriter = (STsdbSnapWriter*)taosMemoryCalloc(1, sizeof(*pWriter));
  if (pWriter == NULL) {
    code = TSDB_CODE_OUT_OF_MEMORY;
    goto _err;
  }
  pWriter->pTsdb = pTsdb;
  pWriter->sver = sver;
  pWriter->ever = ever;

  code = tsdbFSCopy(pTsdb, &pWriter->fs);
  if (code) goto _err;

  // config
  pWriter->minutes = pTsdb->keepCfg.days;
  pWriter->precision = pTsdb->keepCfg.precision;
  pWriter->minRow = pTsdb->pVnode->config.tsdbCfg.minRows;
  pWriter->maxRow = pTsdb->pVnode->config.tsdbCfg.maxRows;
  pWriter->cmprAlg = pTsdb->pVnode->config.tsdbCfg.compression;
  pWriter->commitID = pTsdb->pVnode->state.commitID;

  // for data file
  code = tBlockDataCreate(&pWriter->bData);

  if (code) goto _err;
  pWriter->aBlockIdx = taosArrayInit(0, sizeof(SBlockIdx));
  if (pWriter->aBlockIdx == NULL) {
    code = TSDB_CODE_OUT_OF_MEMORY;
    goto _err;
  }
  code = tBlockDataCreate(&pWriter->bDataR);
  if (code) goto _err;

  pWriter->aSstBlk = taosArrayInit(0, sizeof(SSttBlk));
  if (pWriter->aSstBlk == NULL) {
    code = TSDB_CODE_OUT_OF_MEMORY;
    goto _err;
  }

  pWriter->aBlockIdxW = taosArrayInit(0, sizeof(SBlockIdx));
  if (pWriter->aBlockIdxW == NULL) {
    code = TSDB_CODE_OUT_OF_MEMORY;
    goto _err;
  }
  code = tBlockDataCreate(&pWriter->bDataW);
  if (code) goto _err;

  pWriter->aBlockLW = taosArrayInit(0, sizeof(SSttBlk));
  if (pWriter->aBlockLW == NULL) {
    code = TSDB_CODE_OUT_OF_MEMORY;
    goto _err;
  }

  // for del file
  pWriter->aDelIdxR = taosArrayInit(0, sizeof(SDelIdx));
  if (pWriter->aDelIdxR == NULL) {
    code = TSDB_CODE_OUT_OF_MEMORY;
    goto _err;
  }
  pWriter->aDelData = taosArrayInit(0, sizeof(SDelData));
  if (pWriter->aDelData == NULL) {
    code = TSDB_CODE_OUT_OF_MEMORY;
    goto _err;
  }
  pWriter->aDelIdxW = taosArrayInit(0, sizeof(SDelIdx));
  if (pWriter->aDelIdxW == NULL) {
    code = TSDB_CODE_OUT_OF_MEMORY;
    goto _err;
  }

  *ppWriter = pWriter;

  tsdbInfo("vgId:%d, tsdb snapshot writer open for %s succeed", TD_VID(pTsdb->pVnode), pTsdb->path);
  return code;
_err:
  tsdbError("vgId:%d, tsdb snapshot writer open for %s failed since %s", TD_VID(pTsdb->pVnode), pTsdb->path,
            tstrerror(code));
  *ppWriter = NULL;
  return code;
}

int32_t tsdbSnapWriterClose(STsdbSnapWriter** ppWriter, int8_t rollback) {
  int32_t          code = 0;
  STsdbSnapWriter* pWriter = *ppWriter;

  if (rollback) {
    ASSERT(0);
    // code = tsdbFSRollback(pWriter->pTsdb->pFS);
    // if (code) goto _err;
  } else {
    code = tsdbSnapWriteDataEnd(pWriter);
    if (code) goto _err;

    code = tsdbSnapWriteDelEnd(pWriter);
    if (code) goto _err;

    code = tsdbFSCommit1(pWriter->pTsdb, &pWriter->fs);
    if (code) goto _err;

    code = tsdbFSCommit2(pWriter->pTsdb, &pWriter->fs);
    if (code) goto _err;
  }

  for (int32_t iBuf = 0; iBuf < sizeof(pWriter->aBuf) / sizeof(uint8_t*); iBuf++) {
    tFree(pWriter->aBuf[iBuf]);
  }

  tsdbInfo("vgId:%d, vnode snapshot tsdb writer close for %s", TD_VID(pWriter->pTsdb->pVnode), pWriter->pTsdb->path);
  taosMemoryFree(pWriter);
  *ppWriter = NULL;
  return code;

_err:
  tsdbError("vgId:%d, vnode snapshot tsdb writer close for %s failed since %s", TD_VID(pWriter->pTsdb->pVnode),
            pWriter->pTsdb->path, tstrerror(code));
  taosMemoryFree(pWriter);
  *ppWriter = NULL;
  return code;
}

int32_t tsdbSnapWrite(STsdbSnapWriter* pWriter, uint8_t* pData, uint32_t nData) {
  int32_t       code = 0;
  SSnapDataHdr* pHdr = (SSnapDataHdr*)pData;

  // ts data
  if (pHdr->type == SNAP_DATA_TSDB) {
    code = tsdbSnapWriteData(pWriter, pData, nData);
    if (code) goto _err;

    goto _exit;
  } else {
    if (pWriter->pDataFWriter) {
      code = tsdbSnapWriteDataEnd(pWriter);
      if (code) goto _err;
    }
  }

  // del data
  if (pHdr->type == SNAP_DATA_DEL) {
    code = tsdbSnapWriteDel(pWriter, pData, nData);
    if (code) goto _err;
  }

_exit:
  tsdbDebug("vgId:%d, tsdb snapshot write for %s succeed", TD_VID(pWriter->pTsdb->pVnode), pWriter->pTsdb->path);

  return code;

_err:
  tsdbError("vgId:%d, tsdb snapshot write for %s failed since %s", TD_VID(pWriter->pTsdb->pVnode), pWriter->pTsdb->path,
            tstrerror(code));
  return code;
}
