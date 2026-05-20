#ifndef PALA_WEB_APPS_UPLOAD_H
#define PALA_WEB_APPS_UPLOAD_H

// Mounts /upload-app — multipart streaming receiver for Pala app .bin
// binaries. Files land in /apps/. The handler validates the
// PalaAppHeader magic before committing the temp file, so a non-app
// upload is rejected before it can pollute the catalog.
//
// Call once from registerWebRoutes().
void registerAppUploadRoutes();

// Close any open tmp file and clear all per-session fields. Called by the
// upload screen at session start/stop. Storage lives file-static inside
// apps_upload.cpp.
void resetAppUpload();

#endif  // PALA_WEB_APPS_UPLOAD_H
