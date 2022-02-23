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

#include <gtest/gtest.h>
#include <tglobal.h>
#include <iostream>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wwrite-strings"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wformat"
#pragma GCC diagnostic ignored "-Wint-to-pointer-cast"
#pragma GCC diagnostic ignored "-Wpointer-arith"

#include "os.h"

#include "taos.h"
#include "tdef.h"
#include "tvariant.h"
#include "tep.h"
#include "stub.h"
#include "addr_any.h"
#include "scalar.h"
#include "nodes.h"
#include "tlog.h"
#include "filter.h"

namespace {

int64_t flttLeftV = 21, flttRightV = 10;
double flttLeftVd = 21.0, flttRightVd = 10.0;

void flttInitLogFile() {
  const char   *defaultLogFileNamePrefix = "taoslog";
  const int32_t maxLogFileNum = 10;

  tsAsyncLog = 0;
  qDebugFlag = 159;

  char temp[128] = {0};
  sprintf(temp, "%s/%s", tsLogDir, defaultLogFileNamePrefix);
  if (taosInitLog(temp, tsNumOfLogLines, maxLogFileNum) < 0) {
    printf("failed to open log file in directory:%s\n", tsLogDir);
  }
}


void flttMakeValueNode(SNode **pNode, int32_t dataType, void *value) {
  SNode *node = nodesMakeNode(QUERY_NODE_VALUE);
  SValueNode *vnode = (SValueNode *)node;
  vnode->node.resType.type = dataType;

  if (IS_VAR_DATA_TYPE(dataType)) {
    vnode->datum.p = (char *)malloc(varDataTLen(value));
    varDataCopy(vnode->datum.p, value);
    vnode->node.resType.bytes = varDataLen(value);
  } else {
    vnode->node.resType.bytes = tDataTypes[dataType].bytes;
    assignVal((char *)nodesGetValueFromNode(vnode), (const char *)value, 0, dataType);
  }
  
  *pNode = (SNode *)vnode;
}

void flttMakeColRefNode(SNode **pNode, SSDataBlock **block, int32_t dataType, int32_t dataBytes, int32_t rowNum, void *value) {
  SNode *node = nodesMakeNode(QUERY_NODE_COLUMN_REF);
  SColumnRefNode *rnode = (SColumnRefNode *)node;
  rnode->dataType.type = dataType;
  rnode->dataType.bytes = dataBytes;
  rnode->tupleId = 0;

  if (NULL == block) {
    rnode->slotId = 2;
    rnode->columnId = 55;
    *pNode = (SNode *)rnode;

    return;
  }

  if (NULL == *block) {
    SSDataBlock *res = (SSDataBlock *)calloc(1, sizeof(SSDataBlock));
    res->info.numOfCols = 3;
    res->info.rows = rowNum;
    res->pDataBlock = taosArrayInit(3, sizeof(SColumnInfoData));
    for (int32_t i = 0; i < 2; ++i) {
      SColumnInfoData idata = {{0}};
      idata.info.type  = TSDB_DATA_TYPE_NULL;
      idata.info.bytes = 10;
      idata.info.colId = 0;

      int32_t size = idata.info.bytes * rowNum;
      idata.pData = (char *)calloc(1, size);
      taosArrayPush(res->pDataBlock, &idata);
    }

    SColumnInfoData idata = {{0}};
    idata.info.type  = dataType;
    idata.info.bytes = dataBytes;
    idata.info.colId = 55;
    idata.pData = (char *)value;
    if (IS_VAR_DATA_TYPE(dataType)) {
      idata.varmeta.offset = (int32_t *)calloc(rowNum, sizeof(int32_t));
      for (int32_t i = 0; i < rowNum; ++i) {
        idata.varmeta.offset[i] = (dataBytes + VARSTR_HEADER_SIZE) * i;
      }
    }
    taosArrayPush(res->pDataBlock, &idata);

    rnode->slotId = 2;
    rnode->columnId = 55;

    *block = res;
  } else {
    SSDataBlock *res = *block;
    
    int32_t idx = taosArrayGetSize(res->pDataBlock);
    SColumnInfoData idata = {{0}};
    idata.info.type  = dataType;
    idata.info.bytes = dataBytes;
    idata.info.colId = 55 + idx;
    idata.pData = (char *)value;
    taosArrayPush(res->pDataBlock, &idata);
    
    rnode->slotId = idx;
    rnode->columnId = 55 + idx;
  }

  *pNode = (SNode *)rnode;
}

void flttMakeOpNode(SNode **pNode, EOperatorType opType, int32_t resType, SNode *pLeft, SNode *pRight) {
  SNode *node = nodesMakeNode(QUERY_NODE_OPERATOR);
  SOperatorNode *onode = (SOperatorNode *)node;
  onode->node.resType.type = resType;
  onode->node.resType.bytes = tDataTypes[resType].bytes;
  
  onode->opType = opType;
  onode->pLeft = pLeft;
  onode->pRight = pRight;

  *pNode = (SNode *)onode;
}

void flttMakeLogicNode(SNode **pNode, ELogicConditionType opType, SNode **nodeList, int32_t nodeNum) {
  SNode *node = nodesMakeNode(QUERY_NODE_LOGIC_CONDITION);
  SLogicConditionNode *onode = (SLogicConditionNode *)node;
  onode->condType = opType;
  onode->node.resType.type = TSDB_DATA_TYPE_BOOL;
  onode->node.resType.bytes = sizeof(bool);

  onode->pParameterList = nodesMakeList();
  for (int32_t i = 0; i < nodeNum; ++i) {
    nodesListAppend(onode->pParameterList, nodeList[i]);
  }
  
  *pNode = (SNode *)onode;
}


void flttMakeListNode(SNode **pNode, SNodeList *list, int32_t resType) {
  SNode *node = nodesMakeNode(QUERY_NODE_NODE_LIST);
  SNodeListNode *lnode = (SNodeListNode *)node;
  lnode->dataType.type = resType;
  lnode->pNodeList = list;

  *pNode = (SNode *)lnode;
}


}

TEST(timerangeTest, greater) {
  SNode *pcol = NULL, *pval = NULL, *opNode1 = NULL, *opNode2 = NULL, *logicNode = NULL;
  bool eRes[5] = {false, false, true, true, true};
  SScalarParam res = {0};
  int64_t tsmall = 222, tbig = 333;
  flttMakeColRefNode(&pcol, NULL, TSDB_DATA_TYPE_TIMESTAMP, sizeof(int64_t), 0, NULL);  
  flttMakeValueNode(&pval, TSDB_DATA_TYPE_TIMESTAMP, &tsmall);
  flttMakeOpNode(&opNode1, OP_TYPE_GREATER_THAN, TSDB_DATA_TYPE_BOOL, pcol, pval);

  SFilterInfo *filter = NULL;
  int32_t code = filterInitFromNode(opNode1, &filter, FLT_OPTION_NO_REWRITE|FLT_OPTION_TIMESTAMP);
  ASSERT_EQ(code, 0);
  STimeWindow win = {0};
  code = filterGetTimeRange(filter, &win);
  ASSERT_EQ(code, 0);
  ASSERT_EQ(win.skey, tsmall);
  ASSERT_EQ(win.ekey, INT64_MAX); 
}

TEST(timerangeTest, greater_and_lower) {
  SNode *pcol = NULL, *pval = NULL, *opNode1 = NULL, *opNode2 = NULL, *logicNode = NULL;
  bool eRes[5] = {false, false, true, true, true};
  SScalarParam res = {0};
  int64_t tsmall = 222, tbig = 333;
  flttMakeColRefNode(&pcol, NULL, TSDB_DATA_TYPE_TIMESTAMP, sizeof(int64_t), 0, NULL);  
  flttMakeValueNode(&pval, TSDB_DATA_TYPE_TIMESTAMP, &tsmall);
  flttMakeOpNode(&opNode1, OP_TYPE_GREATER_THAN, TSDB_DATA_TYPE_BOOL, pcol, pval);
  flttMakeColRefNode(&pcol, NULL, TSDB_DATA_TYPE_TIMESTAMP, sizeof(int64_t), 0, NULL);  
  flttMakeValueNode(&pval, TSDB_DATA_TYPE_TIMESTAMP, &tbig);
  flttMakeOpNode(&opNode2, OP_TYPE_LOWER_THAN, TSDB_DATA_TYPE_BOOL, pcol, pval);
  SNode *list[2] = {0};
  list[0] = opNode1;
  list[1] = opNode2;
  
  flttMakeLogicNode(&logicNode, LOGIC_COND_TYPE_AND, list, 2);

  SFilterInfo *filter = NULL;
  int32_t code = filterInitFromNode(logicNode, &filter, FLT_OPTION_NO_REWRITE|FLT_OPTION_TIMESTAMP);
  ASSERT_EQ(code, 0);
  STimeWindow win = {0};
  code = filterGetTimeRange(filter, &win);
  ASSERT_EQ(code, 0);
  ASSERT_EQ(win.skey, tsmall);
  ASSERT_EQ(win.ekey, tbig); 
}


TEST(columnTest, smallint_column_greater_double_value) {
  SNode *pLeft = NULL, *pRight = NULL, *opNode = NULL;
  int16_t leftv[5]= {1, 2, 3, 4, 5};
  double rightv= 2.5;
  int8_t eRes[5] = {0, 0, 1, 1, 1};
  SSDataBlock *src = NULL;
  SScalarParam res = {0};
  int32_t rowNum = sizeof(leftv)/sizeof(leftv[0]);
  flttMakeColRefNode(&pLeft, &src, TSDB_DATA_TYPE_SMALLINT, sizeof(int16_t), rowNum, leftv);
  flttMakeValueNode(&pRight, TSDB_DATA_TYPE_DOUBLE, &rightv);
  flttMakeOpNode(&opNode, OP_TYPE_GREATER_THAN, TSDB_DATA_TYPE_BOOL, pLeft, pRight);
  
  SFilterInfo *filter = NULL;
  int32_t code = filterInitFromNode(opNode, &filter, 0);
  ASSERT_EQ(code, 0);

  SColumnDataAgg stat = {0};
  stat.colId = ((SColumnRefNode *)pLeft)->columnId;
  stat.max = 10;
  stat.min = 5;
  stat.numOfNull = 0;
  bool keep = filterRangeExecute(filter, &stat, 1, rowNum);
  ASSERT_EQ(keep, true);

  stat.max = 1;
  stat.min = -1;
  keep = filterRangeExecute(filter, &stat, 1, rowNum);
  ASSERT_EQ(keep, true);

  stat.max = 10;
  stat.min = 5;
  stat.numOfNull = rowNum;
  keep = filterRangeExecute(filter, &stat, 1, rowNum);
  ASSERT_EQ(keep, true);

  SFilterColumnParam param = {.numOfCols= src->info.numOfCols, .pDataBlock = src->pDataBlock};
  code = filterSetDataFromSlotId(filter, &param);
  ASSERT_EQ(code, 0);

  stat.max = 5;
  stat.min = 1;
  stat.numOfNull = 0;
  int8_t *rowRes = NULL;
  keep = filterExecute(filter, src, &rowRes, &stat, src->info.numOfCols);
  ASSERT_EQ(keep, false);
  
  for (int32_t i = 0; i < rowNum; ++i) {
    ASSERT_EQ(*((int8_t *)rowRes + i), eRes[i]);
  }
}

TEST(columnTest, int_column_greater_smallint_value) {
  SNode *pLeft = NULL, *pRight = NULL, *opNode = NULL;
  int32_t leftv[5]= {1, 3, 5, 7, 9};
  int16_t rightv= 4;
  int8_t eRes[5] = {0, 0, 1, 1, 1};
  SSDataBlock *src = NULL;
  SScalarParam res = {0};
  int32_t rowNum = sizeof(leftv)/sizeof(leftv[0]);
  flttMakeColRefNode(&pLeft, &src, TSDB_DATA_TYPE_INT, sizeof(int32_t), rowNum, leftv);
  flttMakeValueNode(&pRight, TSDB_DATA_TYPE_SMALLINT, &rightv);
  flttMakeOpNode(&opNode, OP_TYPE_GREATER_THAN, TSDB_DATA_TYPE_BOOL, pLeft, pRight);
  
  SFilterInfo *filter = NULL;
  int32_t code = filterInitFromNode(opNode, &filter, 0);
  ASSERT_EQ(code, 0);

  SColumnDataAgg stat = {0};
  stat.colId = ((SColumnRefNode *)pLeft)->columnId;
  stat.max = 10;
  stat.min = 5;
  stat.numOfNull = 0;
  bool keep = filterRangeExecute(filter, &stat, 1, rowNum);
  ASSERT_EQ(keep, true);

  stat.max = 1;
  stat.min = -1;
  keep = filterRangeExecute(filter, &stat, 1, rowNum);
  ASSERT_EQ(keep, false);

  stat.max = 10;
  stat.min = 5;
  stat.numOfNull = rowNum;
  keep = filterRangeExecute(filter, &stat, 1, rowNum);
  ASSERT_EQ(keep, false);

  SFilterColumnParam param = {.numOfCols= src->info.numOfCols, .pDataBlock = src->pDataBlock};
  code = filterSetDataFromSlotId(filter, &param);
  ASSERT_EQ(code, 0);

  stat.max = 5;
  stat.min = 1;
  stat.numOfNull = 0;
  int8_t *rowRes = NULL;
  keep = filterExecute(filter, src, &rowRes, &stat, src->info.numOfCols);
  ASSERT_EQ(keep, false);
  
  for (int32_t i = 0; i < rowNum; ++i) {
    ASSERT_EQ(*((int8_t *)rowRes + i), eRes[i]);
  }
}


TEST(columnTest, int_column_in_double_list) {
  SNode *pLeft = NULL, *pRight = NULL, *listNode = NULL, *opNode = NULL;
  int32_t leftv[5] = {1, 2, 3, 4, 5};
  double rightv1 = 1.1,rightv2 = 2.2,rightv3 = 3.3;
  bool eRes[5] = {true, true, true, false, false};  
  SSDataBlock *src = NULL;  
  SScalarParam res = {0};
  int32_t rowNum = sizeof(leftv)/sizeof(leftv[0]);
  flttMakeColRefNode(&pLeft, &src, TSDB_DATA_TYPE_INT, sizeof(int32_t), rowNum, leftv);  
  SNodeList* list = nodesMakeList();
  flttMakeValueNode(&pRight, TSDB_DATA_TYPE_DOUBLE, &rightv1);
  nodesListAppend(list, pRight);
  flttMakeValueNode(&pRight, TSDB_DATA_TYPE_DOUBLE, &rightv2);
  nodesListAppend(list, pRight);
  flttMakeValueNode(&pRight, TSDB_DATA_TYPE_DOUBLE, &rightv3);
  nodesListAppend(list, pRight);
  flttMakeListNode(&listNode,list, TSDB_DATA_TYPE_INT);
  flttMakeOpNode(&opNode, OP_TYPE_IN, TSDB_DATA_TYPE_BOOL, pLeft, listNode);

  SFilterInfo *filter = NULL;
  int32_t code = filterInitFromNode(opNode, &filter, 0);
  ASSERT_EQ(code, 0);

  SColumnDataAgg stat = {0};
  SFilterColumnParam param = {.numOfCols= src->info.numOfCols, .pDataBlock = src->pDataBlock};
  code = filterSetDataFromSlotId(filter, &param);
  ASSERT_EQ(code, 0);

  stat.max = 5;
  stat.min = 1;
  stat.numOfNull = 0;
  int8_t *rowRes = NULL;
  bool keep = filterExecute(filter, src, &rowRes, &stat, src->info.numOfCols);
  ASSERT_EQ(keep, false);
  
  for (int32_t i = 0; i < rowNum; ++i) {
    ASSERT_EQ(*((int8_t *)rowRes + i), eRes[i]);
  }

  
}



TEST(columnTest, binary_column_in_binary_list) {
  SNode *pLeft = NULL, *pRight = NULL, *listNode = NULL, *opNode = NULL;
  bool eRes[5] = {true, true, false, false, false};  
  SSDataBlock *src = NULL;  
  SScalarParam res = {0};
  char leftv[5][5]= {0};
  char rightv[3][5]= {0};
  for (int32_t i = 0; i < 5; ++i) {
    leftv[i][2] = 'a' + i;
    leftv[i][3] = 'b' + i;
    leftv[i][4] = '0' + i;
    varDataSetLen(leftv[i], 3);
  }  
  for (int32_t i = 0; i < 2; ++i) {
    rightv[i][2] = 'a' + i;
    rightv[i][3] = 'b' + i;
    rightv[i][4] = '0' + i;
    varDataSetLen(rightv[i], 3);
  }  
  for (int32_t i = 2; i < 3; ++i) {
    rightv[i][2] = 'a' + i;
    rightv[i][3] = 'a' + i;
    rightv[i][4] = 'a' + i;
    varDataSetLen(rightv[i], 3);
  }
  
  int32_t rowNum = sizeof(leftv)/sizeof(leftv[0]);
  flttMakeColRefNode(&pLeft, &src, TSDB_DATA_TYPE_BINARY, 3, rowNum, leftv);  
  SNodeList* list = nodesMakeList();
  flttMakeValueNode(&pRight, TSDB_DATA_TYPE_BINARY, rightv[0]);
  nodesListAppend(list, pRight);
  flttMakeValueNode(&pRight, TSDB_DATA_TYPE_BINARY, rightv[1]);
  nodesListAppend(list, pRight);
  flttMakeValueNode(&pRight, TSDB_DATA_TYPE_BINARY, rightv[2]);
  nodesListAppend(list, pRight);
  flttMakeListNode(&listNode,list, TSDB_DATA_TYPE_BINARY);
  flttMakeOpNode(&opNode, OP_TYPE_IN, TSDB_DATA_TYPE_BOOL, pLeft, listNode);
  
  SFilterInfo *filter = NULL;
  int32_t code = filterInitFromNode(opNode, &filter, 0);
  ASSERT_EQ(code, 0);

  SColumnDataAgg stat = {0};
  SFilterColumnParam param = {.numOfCols= src->info.numOfCols, .pDataBlock = src->pDataBlock};
  code = filterSetDataFromSlotId(filter, &param);
  ASSERT_EQ(code, 0);

  stat.max = 5;
  stat.min = 1;
  stat.numOfNull = 0;
  int8_t *rowRes = NULL;
  bool keep = filterExecute(filter, src, &rowRes, &stat, src->info.numOfCols);
  ASSERT_EQ(keep, false);
  
  for (int32_t i = 0; i < rowNum; ++i) {
    ASSERT_EQ(*((int8_t *)rowRes + i), eRes[i]);
  }
}


TEST(columnTest, binary_column_like_binary) {
  SNode *pLeft = NULL, *pRight = NULL, *opNode = NULL;
  char rightv[64] = {0};
  char leftv[5][5]= {0};
  SSDataBlock *src = NULL;  
  SScalarParam res = {0};
  bool eRes[5] = {true, false, true, false, true};  
  
  for (int32_t i = 0; i < 5; ++i) {
    leftv[i][2] = 'a';
    leftv[i][3] = 'a';
    leftv[i][4] = '0' + i % 2;
    varDataSetLen(leftv[i], 3);
  }  
  
  int32_t rowNum = sizeof(leftv)/sizeof(leftv[0]);
  flttMakeColRefNode(&pLeft, &src, TSDB_DATA_TYPE_BINARY, 3, rowNum, leftv);  

  sprintf(&rightv[2], "%s", "__0");
  varDataSetLen(rightv, strlen(&rightv[2]));
  flttMakeValueNode(&pRight, TSDB_DATA_TYPE_BINARY, rightv);
  flttMakeOpNode(&opNode, OP_TYPE_LIKE, TSDB_DATA_TYPE_BOOL, pLeft, pRight);
  
  SFilterInfo *filter = NULL;
  int32_t code = filterInitFromNode(opNode, &filter, 0);
  ASSERT_EQ(code, 0);

  SColumnDataAgg stat = {0};
  SFilterColumnParam param = {.numOfCols= src->info.numOfCols, .pDataBlock = src->pDataBlock};
  code = filterSetDataFromSlotId(filter, &param);
  ASSERT_EQ(code, 0);

  stat.max = 5;
  stat.min = 1;
  stat.numOfNull = 0;
  int8_t *rowRes = NULL;
  bool keep = filterExecute(filter, src, &rowRes, &stat, src->info.numOfCols);
  ASSERT_EQ(keep, false);
  
  for (int32_t i = 0; i < rowNum; ++i) {
    ASSERT_EQ(*((int8_t *)rowRes + i), eRes[i]);
  }
}


TEST(columnTest, binary_column_is_null) {
  SNode *pLeft = NULL, *opNode = NULL;
  char leftv[5][5]= {0};
  SSDataBlock *src = NULL;  
  SScalarParam res = {0};
  bool eRes[5] = {false, false, false, false, true};  
  
  for (int32_t i = 0; i < 4; ++i) {
    leftv[i][2] = '0' + i % 2;
    leftv[i][3] = 'a';
    leftv[i][4] = '0' + i % 2;
    varDataSetLen(leftv[i], 3);
  }  

  setVardataNull(leftv[4], TSDB_DATA_TYPE_BINARY);
  
  int32_t rowNum = sizeof(leftv)/sizeof(leftv[0]);
  flttMakeColRefNode(&pLeft, &src, TSDB_DATA_TYPE_BINARY, 3, rowNum, leftv);  

  flttMakeOpNode(&opNode, OP_TYPE_IS_NULL, TSDB_DATA_TYPE_BOOL, pLeft, NULL);
  
  SFilterInfo *filter = NULL;
  int32_t code = filterInitFromNode(opNode, &filter, 0);
  ASSERT_EQ(code, 0);

  SColumnDataAgg stat = {0};
  SFilterColumnParam param = {.numOfCols= src->info.numOfCols, .pDataBlock = src->pDataBlock};
  code = filterSetDataFromSlotId(filter, &param);
  ASSERT_EQ(code, 0);

  stat.max = 5;
  stat.min = 1;
  stat.numOfNull = 0;
  int8_t *rowRes = NULL;
  bool keep = filterExecute(filter, src, &rowRes, &stat, src->info.numOfCols);
  ASSERT_EQ(keep, false);
  
  for (int32_t i = 0; i < rowNum; ++i) {
    ASSERT_EQ(*((int8_t *)rowRes + i), eRes[i]);
  }
}

TEST(columnTest, binary_column_is_not_null) {
  SNode *pLeft = NULL, *opNode = NULL;
  char leftv[5][5]= {0};
  SSDataBlock *src = NULL;  
  SScalarParam res = {0};
  bool eRes[5] = {true, true, true, true, false};  
  
  for (int32_t i = 0; i < 4; ++i) {
    leftv[i][2] = '0' + i % 2;
    leftv[i][3] = 'a';
    leftv[i][4] = '0' + i % 2;
    varDataSetLen(leftv[i], 3);
  }  

  setVardataNull(leftv[4], TSDB_DATA_TYPE_BINARY);
  
  int32_t rowNum = sizeof(leftv)/sizeof(leftv[0]);
  flttMakeColRefNode(&pLeft, &src, TSDB_DATA_TYPE_BINARY, 3, rowNum, leftv);  

  flttMakeOpNode(&opNode, OP_TYPE_IS_NOT_NULL, TSDB_DATA_TYPE_BOOL, pLeft, NULL);
  
  SFilterInfo *filter = NULL;
  int32_t code = filterInitFromNode(opNode, &filter, 0);
  ASSERT_EQ(code, 0);

  SColumnDataAgg stat = {0};
  SFilterColumnParam param = {.numOfCols= src->info.numOfCols, .pDataBlock = src->pDataBlock};
  code = filterSetDataFromSlotId(filter, &param);
  ASSERT_EQ(code, 0);

  stat.max = 5;
  stat.min = 1;
  stat.numOfNull = 0;
  int8_t *rowRes = NULL;
  bool keep = filterExecute(filter, src, &rowRes, &stat, src->info.numOfCols);
  ASSERT_EQ(keep, false);
  
  for (int32_t i = 0; i < rowNum; ++i) {
    ASSERT_EQ(*((int8_t *)rowRes + i), eRes[i]);
  }
}



TEST(opTest, smallint_column_greater_int_column) {
  flttInitLogFile();

  SNode *pLeft = NULL, *pRight = NULL, *opNode = NULL;
  int16_t leftv[5] = {1, -6, -2, 11, 101};
  int32_t rightv[5]= {0, -5, -4, 23, 100};
  bool eRes[5] = {true, false, true, false, true};
  SSDataBlock *src = NULL;
  SScalarParam res = {0};
  int32_t rowNum = sizeof(rightv)/sizeof(rightv[0]);
  flttMakeColRefNode(&pLeft, &src, TSDB_DATA_TYPE_SMALLINT, sizeof(int16_t), rowNum, leftv);
  flttMakeColRefNode(&pRight, &src, TSDB_DATA_TYPE_INT, sizeof(int32_t), rowNum, rightv);
  flttMakeOpNode(&opNode, OP_TYPE_GREATER_THAN, TSDB_DATA_TYPE_BOOL, pLeft, pRight);
  
  SFilterInfo *filter = NULL;
  int32_t code = filterInitFromNode(opNode, &filter, 0);
  ASSERT_EQ(code, 0);

  SColumnDataAgg stat = {0};
  SFilterColumnParam param = {.numOfCols= src->info.numOfCols, .pDataBlock = src->pDataBlock};
  code = filterSetDataFromSlotId(filter, &param);
  ASSERT_EQ(code, 0);

  stat.max = 5;
  stat.min = 1;
  stat.numOfNull = 0;
  int8_t *rowRes = NULL;
  bool keep = filterExecute(filter, src, &rowRes, &stat, src->info.numOfCols);
  ASSERT_EQ(keep, false);
  
  for (int32_t i = 0; i < rowNum; ++i) {
    ASSERT_EQ(*((int8_t *)rowRes + i), eRes[i]);
  }
}


TEST(opTest, smallint_value_add_int_column) {
  SNode *pLeft = NULL, *pRight = NULL, *opNode = NULL;
  int32_t leftv = 1;
  int16_t rightv[5]= {0, -1, -4, -1, 100};
  bool eRes[5] = {true, false, true, false, true};
  SSDataBlock *src = NULL;
  SScalarParam res = {0};
  int32_t rowNum = sizeof(rightv)/sizeof(rightv[0]);
  flttMakeValueNode(&pLeft, TSDB_DATA_TYPE_INT, &leftv);
  flttMakeColRefNode(&pRight, &src, TSDB_DATA_TYPE_SMALLINT, sizeof(int16_t), rowNum, rightv);
  flttMakeOpNode(&opNode, OP_TYPE_ADD, TSDB_DATA_TYPE_DOUBLE, pLeft, pRight);
  flttMakeOpNode(&opNode, OP_TYPE_IS_TRUE, TSDB_DATA_TYPE_BOOL, opNode, NULL);
  
  SFilterInfo *filter = NULL;
  int32_t code = filterInitFromNode(opNode, &filter, 0);
  ASSERT_EQ(code, 0);

  SColumnDataAgg stat = {0};
  SFilterColumnParam param = {.numOfCols= src->info.numOfCols, .pDataBlock = src->pDataBlock};
  code = filterSetDataFromSlotId(filter, &param);
  ASSERT_EQ(code, 0);

  stat.max = 5;
  stat.min = 1;
  stat.numOfNull = 0;
  int8_t *rowRes = NULL;
  bool keep = filterExecute(filter, src, &rowRes, &stat, src->info.numOfCols);
  ASSERT_EQ(keep, false);
  
  for (int32_t i = 0; i < rowNum; ++i) {
    ASSERT_EQ(*((int8_t *)rowRes + i), eRes[i]);
  }
}



TEST(opTest, bigint_column_multi_binary_column) {
  SNode *pLeft = NULL, *pRight = NULL, *opNode = NULL;
  int64_t leftv[5]= {1, 2, 3, 4, 5};
  char rightv[5][5]= {0};
  for (int32_t i = 0; i < 5; ++i) {
    rightv[i][2] = rightv[i][3] = '0';
    rightv[i][4] = '0' + i;
    varDataSetLen(rightv[i], 3);
  }
  bool eRes[5] = {false, true, true, true, true};
  SSDataBlock *src = NULL;
  SScalarParam res = {0};
  int32_t rowNum = sizeof(rightv)/sizeof(rightv[0]);
  flttMakeColRefNode(&pLeft, &src, TSDB_DATA_TYPE_BIGINT, sizeof(int64_t), rowNum, leftv);
  flttMakeColRefNode(&pRight, &src, TSDB_DATA_TYPE_BINARY, 5, rowNum, rightv);
  flttMakeOpNode(&opNode, OP_TYPE_MULTI, TSDB_DATA_TYPE_DOUBLE, pLeft, pRight);
  flttMakeOpNode(&opNode, OP_TYPE_IS_TRUE, TSDB_DATA_TYPE_BOOL, opNode, NULL);
  
  SFilterInfo *filter = NULL;
  int32_t code = filterInitFromNode(opNode, &filter, 0);
  ASSERT_EQ(code, 0);

  SColumnDataAgg stat = {0};
  SFilterColumnParam param = {.numOfCols= src->info.numOfCols, .pDataBlock = src->pDataBlock};
  code = filterSetDataFromSlotId(filter, &param);
  ASSERT_EQ(code, 0);

  stat.max = 5;
  stat.min = 1;
  stat.numOfNull = 0;
  int8_t *rowRes = NULL;
  bool keep = filterExecute(filter, src, &rowRes, &stat, src->info.numOfCols);
  ASSERT_EQ(keep, false);
  
  for (int32_t i = 0; i < rowNum; ++i) {
    ASSERT_EQ(*((int8_t *)rowRes + i), eRes[i]);
  }
}

TEST(opTest, smallint_column_and_binary_column) {
  SNode *pLeft = NULL, *pRight = NULL, *opNode = NULL;
  int16_t leftv[5]= {1, 2, 3, 4, 5};
  char rightv[5][5]= {0};
  for (int32_t i = 0; i < 5; ++i) {
    rightv[i][2] = rightv[i][3] = '0';
    rightv[i][4] = '0' + i;
    varDataSetLen(rightv[i], 3);
  }
  bool eRes[5] = {false, false, true, false, true};
  SSDataBlock *src = NULL;
  SScalarParam res = {0};
  int32_t rowNum = sizeof(rightv)/sizeof(rightv[0]);
  flttMakeColRefNode(&pLeft, &src, TSDB_DATA_TYPE_SMALLINT, sizeof(int16_t), rowNum, leftv);
  flttMakeColRefNode(&pRight, &src, TSDB_DATA_TYPE_BINARY, 5, rowNum, rightv);
  flttMakeOpNode(&opNode, OP_TYPE_BIT_AND, TSDB_DATA_TYPE_BIGINT, pLeft, pRight);
  flttMakeOpNode(&opNode, OP_TYPE_IS_TRUE, TSDB_DATA_TYPE_BOOL, opNode, NULL);
  
  SFilterInfo *filter = NULL;
  int32_t code = filterInitFromNode(opNode, &filter, 0);
  ASSERT_EQ(code, 0);

  SColumnDataAgg stat = {0};
  SFilterColumnParam param = {.numOfCols= src->info.numOfCols, .pDataBlock = src->pDataBlock};
  code = filterSetDataFromSlotId(filter, &param);
  ASSERT_EQ(code, 0);

  stat.max = 5;
  stat.min = 1;
  stat.numOfNull = 0;
  int8_t *rowRes = NULL;
  bool keep = filterExecute(filter, src, &rowRes, &stat, src->info.numOfCols);
  ASSERT_EQ(keep, false);
  
  for (int32_t i = 0; i < rowNum; ++i) {
    ASSERT_EQ(*((int8_t *)rowRes + i), eRes[i]);
  }
}

TEST(opTest, smallint_column_or_float_column) {
  SNode *pLeft = NULL, *pRight = NULL, *opNode = NULL;
  int16_t leftv[5]= {1, 2, 0, 4, 5};
  float rightv[5]= {2.0, 3.0, 0, 5.2, 6.0};
  bool eRes[5] = {true, true, false, true, true};
  SSDataBlock *src = NULL;
  SScalarParam res = {0};
  int32_t rowNum = sizeof(rightv)/sizeof(rightv[0]);
  flttMakeColRefNode(&pLeft, &src, TSDB_DATA_TYPE_SMALLINT, sizeof(int16_t), rowNum, leftv);
  flttMakeColRefNode(&pRight, &src, TSDB_DATA_TYPE_FLOAT, sizeof(float), rowNum, rightv);
  flttMakeOpNode(&opNode, OP_TYPE_BIT_OR, TSDB_DATA_TYPE_BIGINT, pLeft, pRight);
  flttMakeOpNode(&opNode, OP_TYPE_IS_TRUE, TSDB_DATA_TYPE_BOOL, opNode, NULL);
  
  SFilterInfo *filter = NULL;
  int32_t code = filterInitFromNode(opNode, &filter, 0);
  ASSERT_EQ(code, 0);

  SColumnDataAgg stat = {0};
  SFilterColumnParam param = {.numOfCols= src->info.numOfCols, .pDataBlock = src->pDataBlock};
  code = filterSetDataFromSlotId(filter, &param);
  ASSERT_EQ(code, 0);

  stat.max = 5;
  stat.min = 1;
  stat.numOfNull = 0;
  int8_t *rowRes = NULL;
  bool keep = filterExecute(filter, src, &rowRes, &stat, src->info.numOfCols);
  ASSERT_EQ(keep, false);
  
  for (int32_t i = 0; i < rowNum; ++i) {
    ASSERT_EQ(*((int8_t *)rowRes + i), eRes[i]);
  }
}



TEST(opTest, smallint_column_or_double_value) {
  SNode *pLeft = NULL, *pRight = NULL, *opNode = NULL;
  int16_t leftv[5]= {0, 2, 3, 0, -1};
  double rightv= 10.2;
  bool eRes[5] = {true, true, true, true, true};
  SSDataBlock *src = NULL;
  SScalarParam res = {0};
  int32_t rowNum = sizeof(leftv)/sizeof(leftv[0]);
  flttMakeColRefNode(&pLeft, &src, TSDB_DATA_TYPE_SMALLINT, sizeof(int16_t), rowNum, leftv);
  flttMakeValueNode(&pRight, TSDB_DATA_TYPE_DOUBLE, &rightv);
  flttMakeOpNode(&opNode, OP_TYPE_BIT_OR, TSDB_DATA_TYPE_BIGINT, pLeft, pRight);
  flttMakeOpNode(&opNode, OP_TYPE_IS_TRUE, TSDB_DATA_TYPE_BOOL, opNode, NULL);
  
  SFilterInfo *filter = NULL;
  int32_t code = filterInitFromNode(opNode, &filter, 0);
  ASSERT_EQ(code, 0);

  SColumnDataAgg stat = {0};
  SFilterColumnParam param = {.numOfCols= src->info.numOfCols, .pDataBlock = src->pDataBlock};
  code = filterSetDataFromSlotId(filter, &param);
  ASSERT_EQ(code, 0);

  stat.max = 5;
  stat.min = 1;
  stat.numOfNull = 0;
  int8_t *rowRes = NULL;
  bool keep = filterExecute(filter, src, &rowRes, &stat, src->info.numOfCols);
  ASSERT_EQ(keep, true);
  
  for (int32_t i = 0; i < rowNum; ++i) {
    ASSERT_EQ(*((int8_t *)rowRes + i), eRes[i]);
  }
}


TEST(opTest, binary_column_is_true) {
  SNode *pLeft = NULL, *opNode = NULL;
  char leftv[5][5]= {0};
  SSDataBlock *src = NULL;  
  SScalarParam res = {0};
  bool eRes[5] = {false, true, false, true, false};  
  
  for (int32_t i = 0; i < 5; ++i) {
    leftv[i][2] = '0' + i % 2;
    leftv[i][3] = 'a';
    leftv[i][4] = '0' + i % 2;
    varDataSetLen(leftv[i], 3);
  }  
  
  int32_t rowNum = sizeof(leftv)/sizeof(leftv[0]);
  flttMakeColRefNode(&pLeft, &src, TSDB_DATA_TYPE_BINARY, 3, rowNum, leftv);  

  flttMakeOpNode(&opNode, OP_TYPE_IS_TRUE, TSDB_DATA_TYPE_BOOL, pLeft, NULL);
  
  SFilterInfo *filter = NULL;
  int32_t code = filterInitFromNode(opNode, &filter, 0);
  ASSERT_EQ(code, 0);

  SColumnDataAgg stat = {0};
  SFilterColumnParam param = {.numOfCols= src->info.numOfCols, .pDataBlock = src->pDataBlock};
  code = filterSetDataFromSlotId(filter, &param);
  ASSERT_EQ(code, 0);

  stat.max = 5;
  stat.min = 1;
  stat.numOfNull = 0;
  int8_t *rowRes = NULL;
  bool keep = filterExecute(filter, src, &rowRes, &stat, src->info.numOfCols);
  ASSERT_EQ(keep, false);
  
  for (int32_t i = 0; i < rowNum; ++i) {
    ASSERT_EQ(*((int8_t *)rowRes + i), eRes[i]);
  }
}

#if 0


TEST(logicTest, and_or_and) {

}

TEST(logicTest, or_and_or) {

}



#endif

int main(int argc, char** argv) {
  srand(time(NULL));
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

#pragma GCC diagnostic pop
