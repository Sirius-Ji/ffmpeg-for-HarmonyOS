#ifndef FF_EXTRACT_MEDIA_H
#define FF_EXTRACT_MEDIA_H

#include <string>

// M4.4：流复制提取，不重新编码。type = "video" | "audio"。
int ExtractMedia(const std::string &srcPath, const std::string &dstPath,
                 const std::string &type, std::string *result);

#endif
