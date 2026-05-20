#ifndef PALA_WEB_UPLOAD_H
#define PALA_WEB_UPLOAD_H

// Mounts /upload (book upload, multipart) and /upload-sleep (custom sleep
// image upload). Both register a stream handler (chunk receiver) and a done
// handler (final response).
void registerUploadRoutes();

// Close any open tmp file and clear all per-session fields. Called by the
// upload screen at session start/stop so a stale field from a prior session
// can't leak through. Storage lives file-static inside upload.cpp.
void resetBookUpload();
void resetSleepUpload();

#endif  // PALA_WEB_UPLOAD_H
