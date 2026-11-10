// Accept lists for the file pickers (FolderPicker "mode: 'file'").
// The real-filesystem browsing path (home:true) does not populate mimeType,
// so extensions are the primary filter; mime prefixes are kept for future-proofing.

/** Common document and office formats + PDF. */
export const DOCUMENT_ACCEPT = [
  ".pdf",
  ".txt",
  ".md",
  ".doc",
  ".docx",
  ".odt",
  ".rtf",
  ".xls",
  ".xlsx",
  ".ods",
  ".csv",
  ".ppt",
  ".pptx",
  ".odp",
  "application/pdf",
  "text/",
  "application/msword",
  "application/vnd.openxmlformats-officedocument",
  "application/vnd.oasis.opendocument",
  "application/vnd.ms-excel",
  "application/vnd.ms-powerpoint",
];

/** Common image formats. */
export const IMAGE_ACCEPT = [
  ".png",
  ".jpg",
  ".jpeg",
  ".gif",
  ".webp",
  ".svg",
  ".bmp",
  ".ico",
  ".tiff",
  ".tif",
  ".heic",
  ".avif",
  "image/",
];
