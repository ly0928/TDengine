#define ALLOW_FORBID_FUNC

#include "cos.h"

extern char   tsS3Endpoint[];
extern char   tsS3AccessKeyId[];
extern char   tsS3AccessKeySecret[];
extern char   tsS3BucketName[];
extern char   tsS3AppId[];
extern char   tsS3Hostname[];
extern int8_t tsS3Https;

#if defined(USE_S3)

#include "libs3.h"

static int         verifyPeerG = 0;
static const char *awsRegionG = NULL;
static int         forceG = 0;
static int         showResponsePropertiesG = 0;
static S3Protocol  protocolG = S3ProtocolHTTPS;
//  static S3Protocol protocolG = S3ProtocolHTTP;
static S3UriStyle uriStyleG = S3UriStylePath;
static int        retriesG = 5;
static int        timeoutMsG = 0;

static int32_t s3Begin() {
  S3Status    status;
  const char *hostname = tsS3Hostname;
  const char *env_hn = getenv("S3_HOSTNAME");

  if (env_hn) {
    hostname = env_hn;
  }

  if ((status = S3_initialize("s3", verifyPeerG | S3_INIT_ALL, hostname)) != S3StatusOK) {
    vError("Failed to initialize libs3: %s\n", S3_get_status_name(status));
    return -1;
  }

  protocolG = !tsS3Https;

  return 0;
}

static void s3End() { S3_deinitialize(); }
int32_t     s3Init() { return s3Begin(); }

void s3CleanUp() { s3End(); }

static int should_retry() {
  /*
  if (retriesG--) {
    // Sleep before next retry; start out with a 1 second sleep
    static int retrySleepInterval = 1 * SLEEP_UNITS_PER_SECOND;
    sleep(retrySleepInterval);
    // Next sleep 1 second longer
    retrySleepInterval++;
    return 1;
  }
  */

  return 0;
}

static void s3PrintError(const char *func, S3Status status, char error_details[]) {
  if (status < S3StatusErrorAccessDenied) {
    vError("%s: %s", __func__, S3_get_status_name(status));
  } else {
    vError("%s: %s, %s", __func__, S3_get_status_name(status), error_details);
  }
}

typedef struct {
  char     err_msg[128];
  S3Status status;
  uint64_t content_length;
  char    *buf;
} TS3SizeCBD;

static S3Status responsePropertiesCallback(const S3ResponseProperties *properties, void *callbackData) {
  //(void)callbackData;
  TS3SizeCBD *cbd = callbackData;
  if (properties->contentLength > 0) {
    cbd->content_length = properties->contentLength;
  } else {
    cbd->content_length = 0;
  }

  return S3StatusOK;
}

static void responseCompleteCallback(S3Status status, const S3ErrorDetails *error, void *callbackData) {
  TS3SizeCBD *cbd = callbackData;
  cbd->status = status;

  int       len = 0;
  const int elen = sizeof(cbd->err_msg);
  if (error) {
    if (error->message) {
      len += snprintf(&(cbd->err_msg[len]), elen - len, "  Message: %s\n", error->message);
    }
    if (error->resource) {
      len += snprintf(&(cbd->err_msg[len]), elen - len, "  Resource: %s\n", error->resource);
    }
    if (error->furtherDetails) {
      len += snprintf(&(cbd->err_msg[len]), elen - len, "  Further Details: %s\n", error->furtherDetails);
    }
    if (error->extraDetailsCount) {
      len += snprintf(&(cbd->err_msg[len]), elen - len, "%s", "  Extra Details:\n");
      for (int i = 0; i < error->extraDetailsCount; i++) {
        len += snprintf(&(cbd->err_msg[len]), elen - len, "    %s: %s\n", error->extraDetails[i].name,
                        error->extraDetails[i].value);
      }
    }
  }
}

typedef struct growbuffer {
  // The total number of bytes, and the start byte
  int size;
  // The start byte
  int start;
  // The blocks
  char               data[64 * 1024];
  struct growbuffer *prev, *next;
} growbuffer;

// returns nonzero on success, zero on out of memory
static int growbuffer_append(growbuffer **gb, const char *data, int dataLen) {
  int origDataLen = dataLen;
  while (dataLen) {
    growbuffer *buf = *gb ? (*gb)->prev : 0;
    if (!buf || (buf->size == sizeof(buf->data))) {
      buf = (growbuffer *)malloc(sizeof(growbuffer));
      if (!buf) {
        return 0;
      }
      buf->size = 0;
      buf->start = 0;
      if (*gb && (*gb)->prev) {
        buf->prev = (*gb)->prev;
        buf->next = *gb;
        (*gb)->prev->next = buf;
        (*gb)->prev = buf;
      } else {
        buf->prev = buf->next = buf;
        *gb = buf;
      }
    }

    int toCopy = (sizeof(buf->data) - buf->size);
    if (toCopy > dataLen) {
      toCopy = dataLen;
    }

    memcpy(&(buf->data[buf->size]), data, toCopy);

    buf->size += toCopy, data += toCopy, dataLen -= toCopy;
  }

  return origDataLen;
}

static void growbuffer_read(growbuffer **gb, int amt, int *amtReturn, char *buffer) {
  *amtReturn = 0;

  growbuffer *buf = *gb;

  if (!buf) {
    return;
  }

  *amtReturn = (buf->size > amt) ? amt : buf->size;

  memcpy(buffer, &(buf->data[buf->start]), *amtReturn);

  buf->start += *amtReturn, buf->size -= *amtReturn;

  if (buf->size == 0) {
    if (buf->next == buf) {
      *gb = 0;
    } else {
      *gb = buf->next;
      buf->prev->next = buf->next;
      buf->next->prev = buf->prev;
    }
    free(buf);
    buf = NULL;
  }
}

static void growbuffer_destroy(growbuffer *gb) {
  growbuffer *start = gb;

  while (gb) {
    growbuffer *next = gb->next;
    free(gb);
    gb = (next == start) ? 0 : next;
  }
}

typedef struct put_object_callback_data {
  char     err_msg[128];
  S3Status status;
  // FILE       *infile;
  TdFilePtr   infileFD;
  growbuffer *gb;
  uint64_t    contentLength, originalContentLength;
  uint64_t    totalContentLength, totalOriginalContentLength;
  int         noStatus;
} put_object_callback_data;

#define MULTIPART_CHUNK_SIZE (768 << 20)  // multipart is 768M

typedef struct UploadManager {
  char     err_msg[128];
  S3Status status;
  // used for initial multipart
  char *upload_id;

  // used for upload part object
  char **etags;
  int    next_etags_pos;

  // used for commit Upload
  growbuffer *gb;
  int         remaining;
} UploadManager;

typedef struct list_parts_callback_data {
  char           err_msg[128];
  S3Status       status;
  int            isTruncated;
  char           nextPartNumberMarker[24];
  char           initiatorId[256];
  char           initiatorDisplayName[256];
  char           ownerId[256];
  char           ownerDisplayName[256];
  char           storageClass[256];
  int            partsCount;
  int            handlePartsStart;
  int            allDetails;
  int            noPrint;
  UploadManager *manager;
} list_parts_callback_data;

typedef struct MultipartPartData {
  char                     err_msg[128];
  S3Status                 status;
  put_object_callback_data put_object_data;
  int                      seq;
  UploadManager           *manager;
} MultipartPartData;

static int putObjectDataCallback(int bufferSize, char *buffer, void *callbackData) {
  put_object_callback_data *data = (put_object_callback_data *)callbackData;
  if (data->infileFD == 0) {
    MultipartPartData *mpd = (MultipartPartData *)callbackData;
    data = &mpd->put_object_data;
  }

  int ret = 0;

  if (data->contentLength) {
    int toRead = ((data->contentLength > (unsigned)bufferSize) ? (unsigned)bufferSize : data->contentLength);
    if (data->gb) {
      growbuffer_read(&(data->gb), toRead, &ret, buffer);
    } else if (data->infileFD) {
      // ret = fread(buffer, 1, toRead, data->infile);
      ret = taosReadFile(data->infileFD, buffer, toRead);
    }
  }

  data->contentLength -= ret;
  data->totalContentLength -= ret;
  /* log too many open files
  if (data->contentLength && !data->noStatus) {
    vTrace("%llu bytes remaining ", (unsigned long long)data->totalContentLength);
    vTrace("(%d%% complete) ...\n", (int)(((data->totalOriginalContentLength - data->totalContentLength) * 100) /
                                          data->totalOriginalContentLength));
  }
  */
  return ret;
}

S3Status initial_multipart_callback(const char *upload_id, void *callbackData) {
  UploadManager *manager = (UploadManager *)callbackData;
  manager->upload_id = strdup(upload_id);
  return S3StatusOK;
}

S3Status MultipartResponseProperiesCallback(const S3ResponseProperties *properties, void *callbackData) {
  responsePropertiesCallback(properties, callbackData);

  MultipartPartData *data = (MultipartPartData *)callbackData;
  int                seq = data->seq;
  const char        *etag = properties->eTag;
  data->manager->etags[seq - 1] = strdup(etag);
  data->manager->next_etags_pos = seq;
  return S3StatusOK;
}

static int multipartPutXmlCallback(int bufferSize, char *buffer, void *callbackData) {
  UploadManager *manager = (UploadManager *)callbackData;
  int            ret = 0;

  if (manager->remaining) {
    int toRead = ((manager->remaining > bufferSize) ? bufferSize : manager->remaining);
    growbuffer_read(&(manager->gb), toRead, &ret, buffer);
  }
  manager->remaining -= ret;
  return ret;
}
/*
static S3Status listPartsCallback(int isTruncated, const char *nextPartNumberMarker, const char *initiatorId,
                                  const char *initiatorDisplayName, const char *ownerId, const char *ownerDisplayName,
                                  const char *storageClass, int partsCount, int handlePartsStart,
                                  const S3ListPart *parts, void *callbackData) {
  list_parts_callback_data *data = (list_parts_callback_data *)callbackData;

  data->isTruncated = isTruncated;
  data->handlePartsStart = handlePartsStart;
  UploadManager *manager = data->manager;

  if (nextPartNumberMarker) {
    snprintf(data->nextPartNumberMarker, sizeof(data->nextPartNumberMarker), "%s", nextPartNumberMarker);
  } else {
    data->nextPartNumberMarker[0] = 0;
  }

  if (initiatorId) {
    snprintf(data->initiatorId, sizeof(data->initiatorId), "%s", initiatorId);
  } else {
    data->initiatorId[0] = 0;
  }

  if (initiatorDisplayName) {
    snprintf(data->initiatorDisplayName, sizeof(data->initiatorDisplayName), "%s", initiatorDisplayName);
  } else {
    data->initiatorDisplayName[0] = 0;
  }

  if (ownerId) {
    snprintf(data->ownerId, sizeof(data->ownerId), "%s", ownerId);
  } else {
    data->ownerId[0] = 0;
  }

  if (ownerDisplayName) {
    snprintf(data->ownerDisplayName, sizeof(data->ownerDisplayName), "%s", ownerDisplayName);
  } else {
    data->ownerDisplayName[0] = 0;
  }

  if (storageClass) {
    snprintf(data->storageClass, sizeof(data->storageClass), "%s", storageClass);
  } else {
    data->storageClass[0] = 0;
  }

  if (partsCount && !data->partsCount && !data->noPrint) {
    // printListPartsHeader();
  }

  int i;
  for (i = 0; i < partsCount; i++) {
    const S3ListPart *part = &(parts[i]);
    char              timebuf[256];
    if (data->noPrint) {
      manager->etags[handlePartsStart + i] = strdup(part->eTag);
      manager->next_etags_pos++;
      manager->remaining = manager->remaining - part->size;
    } else {
      time_t t = (time_t)part->lastModified;
      strftime(timebuf, sizeof(timebuf), "%Y-%m-%dT%H:%M:%SZ", gmtime(&t));
      printf("%-30s", timebuf);
      printf("%-15llu", (unsigned long long)part->partNumber);
      printf("%-45s", part->eTag);
      printf("%-15llu\n", (unsigned long long)part->size);
    }
  }

  data->partsCount += partsCount;

  return S3StatusOK;
}

static int try_get_parts_info(const char *bucketName, const char *key, UploadManager *manager) {
  S3BucketContext bucketContext = {0, tsS3BucketName, protocolG, uriStyleG, tsS3AccessKeyId, tsS3AccessKeySecret,
                                   0, awsRegionG};

  S3ListPartsHandler listPartsHandler = {{&responsePropertiesCallback, &responseCompleteCallback}, &listPartsCallback};

  list_parts_callback_data data;

  memset(&data, 0, sizeof(list_parts_callback_data));

  data.partsCount = 0;
  data.allDetails = 0;
  data.manager = manager;
  data.noPrint = 1;
  do {
    data.isTruncated = 0;
    do {
      S3_list_parts(&bucketContext, key, data.nextPartNumberMarker, manager->upload_id, 0, 0, 0, timeoutMsG,
                    &listPartsHandler, &data);
    } while (S3_status_is_retryable(data.status) && should_retry());
    if (data.status != S3StatusOK) {
      break;
    }
  } while (data.isTruncated);

  if (data.status == S3StatusOK) {
    if (!data.partsCount) {
      // printListMultipartHeader(data.allDetails);
    }
  } else {
    s3PrintError(__func__, data.status, data.err_msg);
    return -1;
  }

  return 0;
}
*/
int32_t s3PutObjectFromFile2(const char *file, const char *object) {
  int32_t     code = 0;
  const char *key = object;
  // const char              *uploadId = 0;
  const char              *filename = 0;
  uint64_t                 contentLength = 0;
  const char              *cacheControl = 0, *contentType = 0, *md5 = 0;
  const char              *contentDispositionFilename = 0, *contentEncoding = 0;
  int64_t                  expires = -1;
  S3CannedAcl              cannedAcl = S3CannedAclPrivate;
  int                      metaPropertiesCount = 0;
  S3NameValue              metaProperties[S3_MAX_METADATA_COUNT];
  char                     useServerSideEncryption = 0;
  int                      noStatus = 0;
  put_object_callback_data data;

  // data.infile = 0;
  data.infileFD = NULL;
  data.gb = 0;
  data.noStatus = noStatus;

  if (taosStatFile(file, &contentLength, NULL, NULL) < 0) {
    vError("ERROR: %s Failed to stat file %s: ", __func__, file);
    code = TAOS_SYSTEM_ERROR(errno);
    return code;
  }

  if (!(data.infileFD = taosOpenFile(file, TD_FILE_READ))) {
    vError("ERROR: %s Failed to open file %s: ", __func__, file);
    code = TAOS_SYSTEM_ERROR(errno);
    return code;
  }

  data.totalContentLength = data.totalOriginalContentLength = data.contentLength = data.originalContentLength =
      contentLength;

  S3BucketContext bucketContext = {0, tsS3BucketName, protocolG, uriStyleG, tsS3AccessKeyId, tsS3AccessKeySecret,
                                   0, awsRegionG};

  S3PutProperties putProperties = {contentType,     md5,
                                   cacheControl,    contentDispositionFilename,
                                   contentEncoding, expires,
                                   cannedAcl,       metaPropertiesCount,
                                   metaProperties,  useServerSideEncryption};

  if (contentLength <= MULTIPART_CHUNK_SIZE) {
    S3PutObjectHandler putObjectHandler = {{&responsePropertiesCallback, &responseCompleteCallback},
                                           &putObjectDataCallback};

    do {
      S3_put_object(&bucketContext, key, contentLength, &putProperties, 0, 0, &putObjectHandler, &data);
    } while (S3_status_is_retryable(data.status) && should_retry());

    if (data.infileFD) {
      taosCloseFile(&data.infileFD);
    } else if (data.gb) {
      growbuffer_destroy(data.gb);
    }

    if (data.status != S3StatusOK) {
      s3PrintError(__func__, data.status, data.err_msg);
      code = TAOS_SYSTEM_ERROR(EIO);
    } else if (data.contentLength) {
      vError("ERROR: %s Failed to read remaining %llu bytes from input", __func__,
             (unsigned long long)data.contentLength);
      code = TAOS_SYSTEM_ERROR(EIO);
    }
  } else {
    uint64_t      totalContentLength = contentLength;
    uint64_t      todoContentLength = contentLength;
    UploadManager manager;
    manager.upload_id = 0;
    manager.gb = 0;

    // div round up
    int      seq;
    uint64_t chunk_size = MULTIPART_CHUNK_SIZE >> 8;
    int      totalSeq = ((contentLength + chunk_size - 1) / chunk_size);

    MultipartPartData partData;
    memset(&partData, 0, sizeof(MultipartPartData));
    int partContentLength = 0;

    S3MultipartInitialHandler handler = {{&responsePropertiesCallback, &responseCompleteCallback},
                                         &initial_multipart_callback};

    S3PutObjectHandler putObjectHandler = {{&MultipartResponseProperiesCallback, &responseCompleteCallback},
                                           &putObjectDataCallback};

    S3MultipartCommitHandler commit_handler = {
        {&responsePropertiesCallback, &responseCompleteCallback}, &multipartPutXmlCallback, 0};

    manager.etags = (char **)taosMemoryMalloc(sizeof(char *) * totalSeq);
    manager.next_etags_pos = 0;
    /*
    if (uploadId) {
      manager.upload_id = strdup(uploadId);
      manager.remaining = contentLength;
      if (!try_get_parts_info(tsS3BucketName, key, &manager)) {
        fseek(data.infile, -(manager.remaining), 2);
        taosLSeekFile(data.infileFD, -(manager.remaining), SEEK_END);
        contentLength = manager.remaining;
        goto upload;
      } else {
        goto clean;
      }
    }
    */
    do {
      S3_initiate_multipart(&bucketContext, key, 0, &handler, 0, timeoutMsG, &manager);
    } while (S3_status_is_retryable(manager.status) && should_retry());

    if (manager.upload_id == 0 || manager.status != S3StatusOK) {
      s3PrintError(__func__, manager.status, manager.err_msg);
      code = TAOS_SYSTEM_ERROR(EIO);
      goto clean;
    }

  upload:
    todoContentLength -= chunk_size * manager.next_etags_pos;
    for (seq = manager.next_etags_pos + 1; seq <= totalSeq; seq++) {
      partData.manager = &manager;
      partData.seq = seq;
      if (partData.put_object_data.gb == NULL) {
        partData.put_object_data = data;
      }
      partContentLength = ((contentLength > chunk_size) ? chunk_size : contentLength);
      // printf("%s Part Seq %d, length=%d\n", srcSize ? "Copying" : "Sending", seq, partContentLength);
      partData.put_object_data.contentLength = partContentLength;
      partData.put_object_data.originalContentLength = partContentLength;
      partData.put_object_data.totalContentLength = todoContentLength;
      partData.put_object_data.totalOriginalContentLength = totalContentLength;
      putProperties.md5 = 0;
      do {
        S3_upload_part(&bucketContext, key, &putProperties, &putObjectHandler, seq, manager.upload_id,
                       partContentLength, 0, timeoutMsG, &partData);
      } while (S3_status_is_retryable(partData.status) && should_retry());
      if (partData.status != S3StatusOK) {
        s3PrintError(__func__, partData.status, partData.err_msg);
        code = TAOS_SYSTEM_ERROR(EIO);
        goto clean;
      }
      contentLength -= chunk_size;
      todoContentLength -= chunk_size;
    }

    int i;
    int size = 0;
    size += growbuffer_append(&(manager.gb), "<CompleteMultipartUpload>", strlen("<CompleteMultipartUpload>"));
    char buf[256];
    int  n;
    for (i = 0; i < totalSeq; i++) {
      n = snprintf(buf, sizeof(buf),
                   "<Part><PartNumber>%d</PartNumber>"
                   "<ETag>%s</ETag></Part>",
                   i + 1, manager.etags[i]);
      size += growbuffer_append(&(manager.gb), buf, n);
    }
    size += growbuffer_append(&(manager.gb), "</CompleteMultipartUpload>", strlen("</CompleteMultipartUpload>"));
    manager.remaining = size;

    do {
      S3_complete_multipart_upload(&bucketContext, key, &commit_handler, manager.upload_id, manager.remaining, 0,
                                   timeoutMsG, &manager);
    } while (S3_status_is_retryable(manager.status) && should_retry());
    if (manager.status != S3StatusOK) {
      s3PrintError(__func__, manager.status, manager.err_msg);
      code = TAOS_SYSTEM_ERROR(EIO);
      goto clean;
    }

  clean:
    if (manager.upload_id) {
      taosMemoryFree(manager.upload_id);
    }
    for (i = 0; i < manager.next_etags_pos; i++) {
      taosMemoryFree(manager.etags[i]);
    }
    growbuffer_destroy(manager.gb);
    taosMemoryFree(manager.etags);
  }

  return code;
}

typedef struct list_bucket_callback_data {
  char     err_msg[128];
  S3Status status;
  int      isTruncated;
  char     nextMarker[1024];
  int      keyCount;
  int      allDetails;
  SArray  *objectArray;
} list_bucket_callback_data;

static S3Status listBucketCallback(int isTruncated, const char *nextMarker, int contentsCount,
                                   const S3ListBucketContent *contents, int commonPrefixesCount,
                                   const char **commonPrefixes, void *callbackData) {
  list_bucket_callback_data *data = (list_bucket_callback_data *)callbackData;

  data->isTruncated = isTruncated;
  if ((!nextMarker || !nextMarker[0]) && contentsCount) {
    nextMarker = contents[contentsCount - 1].key;
  }
  if (nextMarker) {
    snprintf(data->nextMarker, sizeof(data->nextMarker), "%s", nextMarker);
  } else {
    data->nextMarker[0] = 0;
  }

  if (contentsCount && !data->keyCount) {
    // printListBucketHeader(data->allDetails);
  }

  int i;
  for (i = 0; i < contentsCount; ++i) {
    const S3ListBucketContent *content = &(contents[i]);
    // printf("%-50s", content->key);
    char *object_key = strdup(content->key);
    taosArrayPush(data->objectArray, &object_key);
  }
  data->keyCount += contentsCount;

  for (i = 0; i < commonPrefixesCount; i++) {
    // printf("\nCommon Prefix: %s\n", commonPrefixes[i]);
  }

  return S3StatusOK;
}

static void s3FreeObjectKey(void *pItem) {
  char *key = *(char **)pItem;
  taosMemoryFree(key);
}

void s3DeleteObjectsByPrefix(const char *prefix) {
  S3BucketContext     bucketContext = {0, tsS3BucketName, protocolG, uriStyleG, tsS3AccessKeyId, tsS3AccessKeySecret,
                                       0, awsRegionG};
  S3ListBucketHandler listBucketHandler = {{&responsePropertiesCallback, &responseCompleteCallback},
                                           &listBucketCallback};

  const char               *marker = 0, *delimiter = 0;
  int                       maxkeys = 0, allDetails = 0;
  list_bucket_callback_data data;
  data.objectArray = taosArrayInit(32, POINTER_BYTES);
  if (!data.objectArray) {
    vError("%s: %s", __func__, "out of memoty");
    return;
  }
  if (marker) {
    snprintf(data.nextMarker, sizeof(data.nextMarker), "%s", marker);
  } else {
    data.nextMarker[0] = 0;
  }
  data.keyCount = 0;
  data.allDetails = allDetails;

  do {
    data.isTruncated = 0;
    do {
      S3_list_bucket(&bucketContext, prefix, data.nextMarker, delimiter, maxkeys, 0, timeoutMsG, &listBucketHandler,
                     &data);
    } while (S3_status_is_retryable(data.status) && should_retry());
    if (data.status != S3StatusOK) {
      break;
    }
  } while (data.isTruncated && (!maxkeys || (data.keyCount < maxkeys)));

  if (data.status == S3StatusOK) {
    if (data.keyCount > 0) {
      // printListBucketHeader(allDetails);
      s3DeleteObjects(TARRAY_DATA(data.objectArray), TARRAY_SIZE(data.objectArray));
    }
  } else {
    s3PrintError(__func__, data.status, data.err_msg);
  }

  taosArrayDestroyEx(data.objectArray, s3FreeObjectKey);
}

void s3DeleteObjects(const char *object_name[], int nobject) {
  int               status = 0;
  S3BucketContext   bucketContext = {0, tsS3BucketName, protocolG, uriStyleG, tsS3AccessKeyId, tsS3AccessKeySecret,
                                     0, awsRegionG};
  S3ResponseHandler responseHandler = {0, &responseCompleteCallback};

  for (int i = 0; i < nobject; ++i) {
    TS3SizeCBD cbd = {0};
    do {
      S3_delete_object(&bucketContext, object_name[i], 0, timeoutMsG, &responseHandler, &cbd);
    } while (S3_status_is_retryable(cbd.status) && should_retry());

    if ((cbd.status != S3StatusOK) && (cbd.status != S3StatusErrorPreconditionFailed)) {
      s3PrintError(__func__, cbd.status, cbd.err_msg);
    }
  }
}

static S3Status getObjectDataCallback(int bufferSize, const char *buffer, void *callbackData) {
  TS3SizeCBD *cbd = callbackData;
  if (cbd->content_length != bufferSize) {
    cbd->status = S3StatusAbortedByCallback;
    return S3StatusAbortedByCallback;
  }

  char *buf = taosMemoryCalloc(1, bufferSize);
  if (buf) {
    memcpy(buf, buffer, bufferSize);
    cbd->buf = buf;
    cbd->status = S3StatusOK;
    return S3StatusOK;
  } else {
    cbd->status = S3StatusAbortedByCallback;
    return S3StatusAbortedByCallback;
  }
}

int32_t s3GetObjectBlock(const char *object_name, int64_t offset, int64_t size, uint8_t **ppBlock) {
  int         status = 0;
  int64_t     ifModifiedSince = -1, ifNotModifiedSince = -1;
  const char *ifMatch = 0, *ifNotMatch = 0;

  S3BucketContext    bucketContext = {0, tsS3BucketName, protocolG, uriStyleG, tsS3AccessKeyId, tsS3AccessKeySecret,
                                      0, awsRegionG};
  S3GetConditions    getConditions = {ifModifiedSince, ifNotModifiedSince, ifMatch, ifNotMatch};
  S3GetObjectHandler getObjectHandler = {{&responsePropertiesCallback, &responseCompleteCallback},
                                         &getObjectDataCallback};

  TS3SizeCBD cbd = {0};
  cbd.content_length = size;
  do {
    S3_get_object(&bucketContext, object_name, &getConditions, offset, size, 0, 0, &getObjectHandler, &cbd);
  } while (S3_status_is_retryable(cbd.status) && should_retry());

  if (cbd.status != S3StatusOK) {
    vError("%s: %d(%s)", __func__, cbd.status, cbd.err_msg);
    return TAOS_SYSTEM_ERROR(EIO);
  }

  *ppBlock = cbd.buf;

  return 0;
}

long s3Size(const char *object_name) {
  long size = 0;
  int  status = 0;

  S3BucketContext bucketContext = {0, tsS3BucketName, protocolG, uriStyleG, tsS3AccessKeyId, tsS3AccessKeySecret,
                                   0, awsRegionG};

  S3ResponseHandler responseHandler = {&responsePropertiesCallback, &responseCompleteCallback};

  TS3SizeCBD cbd = {0};
  do {
    S3_head_object(&bucketContext, object_name, 0, 0, &responseHandler, &cbd);
  } while (S3_status_is_retryable(cbd.status) && should_retry());

  if ((cbd.status != S3StatusOK) && (cbd.status != S3StatusErrorPreconditionFailed)) {
    vError("%s: %d(%s)", __func__, cbd.status, cbd.err_msg);
  }

  size = cbd.content_length;

  return size;
}

void s3EvictCache(const char *path, long object_size) {}

#elif defined(USE_COS)

#include "cos_api.h"
#include "cos_http_io.h"
#include "cos_log.h"

int32_t s3Init() {
  if (cos_http_io_initialize(NULL, 0) != COSE_OK) {
    return -1;
  }

  // set log level, default COS_LOG_WARN
  cos_log_set_level(COS_LOG_WARN);

  // set log output, default stderr
  cos_log_set_output(NULL);

  return 0;
}

void s3CleanUp() { cos_http_io_deinitialize(); }

static void log_status(cos_status_t *s) {
  cos_warn_log("status->code: %d", s->code);
  if (s->error_code) cos_warn_log("status->error_code: %s", s->error_code);
  if (s->error_msg) cos_warn_log("status->error_msg: %s", s->error_msg);
  if (s->req_id) cos_warn_log("status->req_id: %s", s->req_id);
}

static void s3InitRequestOptions(cos_request_options_t *options, int is_cname) {
  options->config = cos_config_create(options->pool);

  cos_config_t *config = options->config;

  cos_str_set(&config->endpoint, tsS3Endpoint);
  cos_str_set(&config->access_key_id, tsS3AccessKeyId);
  cos_str_set(&config->access_key_secret, tsS3AccessKeySecret);
  cos_str_set(&config->appid, tsS3AppId);

  config->is_cname = is_cname;

  options->ctl = cos_http_controller_create(options->pool, 0);
}

int32_t s3PutObjectFromFile(const char *file_str, const char *object_str) {
  int32_t                code = 0;
  cos_pool_t            *p = NULL;
  int                    is_cname = 0;
  cos_status_t          *s = NULL;
  cos_request_options_t *options = NULL;
  cos_string_t           bucket, object, file;
  cos_table_t           *resp_headers;
  // int                    traffic_limit = 0;

  cos_pool_create(&p, NULL);
  options = cos_request_options_create(p);
  s3InitRequestOptions(options, is_cname);
  cos_table_t *headers = NULL;
  /*
  if (traffic_limit) {
    // 限速值设置范围为819200 - 838860800，即100KB/s - 100MB/s，如果超出该范围将返回400错误
    headers = cos_table_make(p, 1);
    cos_table_add_int(headers, "x-cos-traffic-limit", 819200);
  }
  */
  cos_str_set(&bucket, tsS3BucketName);
  cos_str_set(&file, file_str);
  cos_str_set(&object, object_str);
  s = cos_put_object_from_file(options, &bucket, &object, &file, headers, &resp_headers);
  log_status(s);

  cos_pool_destroy(p);

  if (s->code != 200) {
    return code = s->code;
  }

  return code;
}

int32_t s3PutObjectFromFile2(const char *file_str, const char *object_str) {
  int32_t                     code = 0;
  cos_pool_t                 *p = NULL;
  int                         is_cname = 0;
  cos_status_t               *s = NULL;
  cos_request_options_t      *options = NULL;
  cos_string_t                bucket, object, file;
  cos_table_t                *resp_headers;
  int                         traffic_limit = 0;
  cos_table_t                *headers = NULL;
  cos_resumable_clt_params_t *clt_params = NULL;

  cos_pool_create(&p, NULL);
  options = cos_request_options_create(p);
  s3InitRequestOptions(options, is_cname);
  headers = cos_table_make(p, 0);
  cos_str_set(&bucket, tsS3BucketName);
  cos_str_set(&file, file_str);
  cos_str_set(&object, object_str);

  // upload
  clt_params = cos_create_resumable_clt_params_content(p, 1024 * 1024, 8, COS_FALSE, NULL);
  s = cos_resumable_upload_file(options, &bucket, &object, &file, headers, NULL, clt_params, NULL, &resp_headers, NULL);

  log_status(s);
  if (!cos_status_is_ok(s)) {
    vError("s3: %d(%s)", s->code, s->error_msg);
    vError("%s failed at line %d since %s", __func__, __LINE__, tstrerror(terrno));
    code = TAOS_SYSTEM_ERROR(EIO);
    return code;
  }

  cos_pool_destroy(p);

  if (s->code != 200) {
    return code = s->code;
  }

  return code;
}

void s3DeleteObjectsByPrefix(const char *prefix_str) {
  cos_pool_t            *p = NULL;
  cos_request_options_t *options = NULL;
  int                    is_cname = 0;
  cos_string_t           bucket;
  cos_status_t          *s = NULL;
  cos_string_t           prefix;

  cos_pool_create(&p, NULL);
  options = cos_request_options_create(p);
  s3InitRequestOptions(options, is_cname);
  cos_str_set(&bucket, tsS3BucketName);
  cos_str_set(&prefix, prefix_str);

  s = cos_delete_objects_by_prefix(options, &bucket, &prefix);
  log_status(s);
  cos_pool_destroy(p);
}

void s3DeleteObjects(const char *object_name[], int nobject) {
  cos_pool_t            *p = NULL;
  int                    is_cname = 0;
  cos_string_t           bucket;
  cos_table_t           *resp_headers = NULL;
  cos_request_options_t *options = NULL;
  cos_list_t             object_list;
  cos_list_t             deleted_object_list;
  int                    is_quiet = COS_TRUE;

  cos_pool_create(&p, NULL);
  options = cos_request_options_create(p);
  s3InitRequestOptions(options, is_cname);
  cos_str_set(&bucket, tsS3BucketName);

  cos_list_init(&object_list);
  cos_list_init(&deleted_object_list);

  for (int i = 0; i < nobject; ++i) {
    cos_object_key_t *content = cos_create_cos_object_key(p);
    cos_str_set(&content->key, object_name[i]);
    cos_list_add_tail(&content->node, &object_list);
  }

  cos_status_t *s = cos_delete_objects(options, &bucket, &object_list, is_quiet, &resp_headers, &deleted_object_list);
  log_status(s);

  cos_pool_destroy(p);

  if (cos_status_is_ok(s)) {
    cos_warn_log("delete objects succeeded\n");
  } else {
    cos_warn_log("delete objects failed\n");
  }
}

bool s3Exists(const char *object_name) {
  bool                      ret = false;
  cos_pool_t               *p = NULL;
  int                       is_cname = 0;
  cos_status_t             *s = NULL;
  cos_request_options_t    *options = NULL;
  cos_string_t              bucket;
  cos_string_t              object;
  cos_table_t              *resp_headers;
  cos_table_t              *headers = NULL;
  cos_object_exist_status_e object_exist;

  cos_pool_create(&p, NULL);
  options = cos_request_options_create(p);
  s3InitRequestOptions(options, is_cname);
  cos_str_set(&bucket, tsS3BucketName);
  cos_str_set(&object, object_name);

  s = cos_check_object_exist(options, &bucket, &object, headers, &object_exist, &resp_headers);
  if (object_exist == COS_OBJECT_NON_EXIST) {
    cos_warn_log("object: %.*s non exist.\n", object.len, object.data);
  } else if (object_exist == COS_OBJECT_EXIST) {
    ret = true;
    cos_warn_log("object: %.*s exist.\n", object.len, object.data);
  } else {
    cos_warn_log("object: %.*s unknown status.\n", object.len, object.data);
    log_status(s);
  }

  cos_pool_destroy(p);

  return ret;
}

bool s3Get(const char *object_name, const char *path) {
  bool                   ret = false;
  cos_pool_t            *p = NULL;
  int                    is_cname = 0;
  cos_status_t          *s = NULL;
  cos_request_options_t *options = NULL;
  cos_string_t           bucket;
  cos_string_t           object;
  cos_string_t           file;
  cos_table_t           *resp_headers = NULL;
  cos_table_t           *headers = NULL;
  int                    traffic_limit = 0;

  //创建内存池
  cos_pool_create(&p, NULL);

  //初始化请求选项
  options = cos_request_options_create(p);
  s3InitRequestOptions(options, is_cname);
  cos_str_set(&bucket, tsS3BucketName);
  if (traffic_limit) {
    //限速值设置范围为819200 - 838860800，即100KB/s - 100MB/s，如果超出该范围将返回400错误
    headers = cos_table_make(p, 1);
    cos_table_add_int(headers, "x-cos-traffic-limit", 819200);
  }

  //下载对象
  cos_str_set(&file, path);
  cos_str_set(&object, object_name);
  s = cos_get_object_to_file(options, &bucket, &object, headers, NULL, &file, &resp_headers);
  if (cos_status_is_ok(s)) {
    ret = true;
    cos_warn_log("get object succeeded\n");
  } else {
    cos_warn_log("get object failed\n");
  }

  //销毁内存池
  cos_pool_destroy(p);

  return ret;
}

int32_t s3GetObjectBlock(const char *object_name, int64_t offset, int64_t block_size, uint8_t **ppBlock) {
  int32_t                code = 0;
  cos_pool_t            *p = NULL;
  int                    is_cname = 0;
  cos_status_t          *s = NULL;
  cos_request_options_t *options = NULL;
  cos_string_t           bucket;
  cos_string_t           object;
  cos_table_t           *resp_headers;
  cos_table_t           *headers = NULL;
  cos_buf_t             *content = NULL;
  // cos_string_t file;
  // int  traffic_limit = 0;
  char range_buf[64];

  //创建内存池
  cos_pool_create(&p, NULL);

  //初始化请求选项
  options = cos_request_options_create(p);
  // init_test_request_options(options, is_cname);
  s3InitRequestOptions(options, is_cname);
  cos_str_set(&bucket, tsS3BucketName);
  cos_str_set(&object, object_name);
  cos_list_t download_buffer;
  cos_list_init(&download_buffer);
  /*
  if (traffic_limit) {
    // 限速值设置范围为819200 - 838860800，单位默认为 bit/s，即800Kb/s - 800Mb/s，如果超出该范围将返回400错误
    headers = cos_table_make(p, 1);
    cos_table_add_int(headers, "x-cos-traffic-limit", 819200);
  }
  */

  headers = cos_table_create_if_null(options, headers, 1);
  apr_snprintf(range_buf, sizeof(range_buf), "bytes=%" APR_INT64_T_FMT "-%" APR_INT64_T_FMT, offset,
               offset + block_size - 1);
  apr_table_add(headers, COS_RANGE, range_buf);

  s = cos_get_object_to_buffer(options, &bucket, &object, headers, NULL, &download_buffer, &resp_headers);
  log_status(s);
  if (!cos_status_is_ok(s)) {
    vError("s3: %d(%s)", s->code, s->error_msg);
    vError("%s failed at line %d since %s", __func__, __LINE__, tstrerror(terrno));
    code = TAOS_SYSTEM_ERROR(EIO);
    return code;
  }

  // print_headers(resp_headers);
  int64_t len = 0;
  int64_t size = 0;
  int64_t pos = 0;
  cos_list_for_each_entry(cos_buf_t, content, &download_buffer, node) { len += cos_buf_size(content); }
  // char *buf = cos_pcalloc(p, (apr_size_t)(len + 1));
  char *buf = taosMemoryCalloc(1, (apr_size_t)(len));
  // buf[len] = '\0';
  cos_list_for_each_entry(cos_buf_t, content, &download_buffer, node) {
    size = cos_buf_size(content);
    memcpy(buf + pos, content->pos, (size_t)size);
    pos += size;
  }
  // cos_warn_log("Download data=%s", buf);

  //销毁内存池
  cos_pool_destroy(p);

  *ppBlock = buf;

  return code;
}

typedef struct {
  int64_t size;
  int32_t atime;
  char    name[TSDB_FILENAME_LEN];
} SEvictFile;

static int32_t evictFileCompareAsce(const void *pLeft, const void *pRight) {
  SEvictFile *lhs = (SEvictFile *)pLeft;
  SEvictFile *rhs = (SEvictFile *)pRight;
  return lhs->atime < rhs->atime ? -1 : 1;
}

void s3EvictCache(const char *path, long object_size) {
  SDiskSize disk_size = {0};
  char      dir_name[TSDB_FILENAME_LEN] = "\0";

  tstrncpy(dir_name, path, TSDB_FILENAME_LEN);
  taosDirName(dir_name);

  if (taosGetDiskSize((char *)dir_name, &disk_size) < 0) {
    terrno = TAOS_SYSTEM_ERROR(errno);
    vError("failed to get disk:%s size since %s", path, terrstr());
    return;
  }

  if (object_size >= disk_size.avail - (1 << 30)) {
    // evict too old files
    // 1, list data files' atime under dir(path)
    tdbDirPtr pDir = taosOpenDir(dir_name);
    if (pDir == NULL) {
      terrno = TAOS_SYSTEM_ERROR(errno);
      vError("failed to open %s since %s", dir_name, terrstr());
    }
    SArray        *evict_files = taosArrayInit(16, sizeof(SEvictFile));
    tdbDirEntryPtr pDirEntry;
    while ((pDirEntry = taosReadDir(pDir)) != NULL) {
      char *name = taosGetDirEntryName(pDirEntry);
      if (!strncmp(name + strlen(name) - 5, ".data", 5)) {
        SEvictFile e_file = {0};
        char       entry_name[TSDB_FILENAME_LEN] = "\0";
        int        dir_len = strlen(dir_name);

        memcpy(e_file.name, dir_name, dir_len);
        e_file.name[dir_len] = '/';
        memcpy(e_file.name + dir_len + 1, name, strlen(name));

        taosStatFile(e_file.name, &e_file.size, NULL, &e_file.atime);

        taosArrayPush(evict_files, &e_file);
      }
    }
    taosCloseDir(&pDir);

    // 2, sort by atime
    taosArraySort(evict_files, evictFileCompareAsce);

    // 3, remove files ascendingly until we get enough object_size space
    long   evict_size = 0;
    size_t ef_size = TARRAY_SIZE(evict_files);
    for (size_t i = 0; i < ef_size; ++i) {
      SEvictFile *evict_file = taosArrayGet(evict_files, i);
      taosRemoveFile(evict_file->name);
      evict_size += evict_file->size;
      if (evict_size >= object_size) {
        break;
      }
    }

    taosArrayDestroy(evict_files);
  }
}

long s3Size(const char *object_name) {
  long size = 0;

  cos_pool_t            *p = NULL;
  int                    is_cname = 0;
  cos_status_t          *s = NULL;
  cos_request_options_t *options = NULL;
  cos_string_t           bucket;
  cos_string_t           object;
  cos_table_t           *resp_headers = NULL;

  //创建内存池
  cos_pool_create(&p, NULL);

  //初始化请求选项
  options = cos_request_options_create(p);
  s3InitRequestOptions(options, is_cname);
  cos_str_set(&bucket, tsS3BucketName);

  //获取对象元数据
  cos_str_set(&object, object_name);
  s = cos_head_object(options, &bucket, &object, NULL, &resp_headers);
  // print_headers(resp_headers);
  if (cos_status_is_ok(s)) {
    char *content_length_str = (char *)apr_table_get(resp_headers, COS_CONTENT_LENGTH);
    if (content_length_str != NULL) {
      size = atol(content_length_str);
    }
    cos_warn_log("head object succeeded: %ld\n", size);
  } else {
    cos_warn_log("head object failed\n");
  }

  //销毁内存池
  cos_pool_destroy(p);

  return size;
}

#else

int32_t s3Init() { return 0; }
void    s3CleanUp() {}
int32_t s3PutObjectFromFile(const char *file, const char *object) { return 0; }
int32_t s3PutObjectFromFile2(const char *file, const char *object) { return 0; }
void    s3DeleteObjectsByPrefix(const char *prefix) {}
void    s3DeleteObjects(const char *object_name[], int nobject) {}
bool    s3Exists(const char *object_name) { return false; }
bool    s3Get(const char *object_name, const char *path) { return false; }
int32_t s3GetObjectBlock(const char *object_name, int64_t offset, int64_t size, uint8_t **ppBlock) { return 0; }
void    s3EvictCache(const char *path, long object_size) {}
long    s3Size(const char *object_name) { return 0; }

#endif