// MIT License
//
// Copyright (c) 2026 Christian Spoo
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include <mntent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

FILE *setmntent(const char *filename, const char *type) {
    return fopen(filename, type);
}

int endmntent(FILE *stream) {
    if (stream)
        fclose(stream);
    return 1;
}

struct mntent *getmntent_r(FILE *stream, struct mntent *result,
                            char *buf, int buflen) {
    while (fgets(buf, buflen, stream)) {
        char *line = buf;
        while (*line == ' ' || *line == '\t') line++;
        if (*line == '#' || *line == '\n' || *line == '\0')
            continue;

        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';

        char *save;
        result->mnt_fsname = strtok_r(line, " \t", &save);
        result->mnt_dir    = strtok_r(NULL, " \t", &save);
        result->mnt_type   = strtok_r(NULL, " \t", &save);
        result->mnt_opts   = strtok_r(NULL, " \t", &save);
        char *freq         = strtok_r(NULL, " \t", &save);
        char *passno       = strtok_r(NULL, " \t", &save);

        if (!result->mnt_fsname || !result->mnt_dir ||
            !result->mnt_type   || !result->mnt_opts)
            continue;

        result->mnt_freq   = freq   ? atoi(freq)   : 0;
        result->mnt_passno = passno ? atoi(passno) : 0;
        return result;
    }
    return NULL;
}

struct mntent *getmntent(FILE *stream) {
    static struct mntent result;
    static char buf[512];
    return getmntent_r(stream, &result, buf, sizeof(buf));
}

int addmntent(FILE *stream, const struct mntent *mnt) {
    if (fseek(stream, 0, SEEK_END) < 0)
        return 1;
    return fprintf(stream, "%s %s %s %s %d %d\n",
                   mnt->mnt_fsname, mnt->mnt_dir, mnt->mnt_type,
                   mnt->mnt_opts, mnt->mnt_freq, mnt->mnt_passno) < 0 ? 1 : 0;
}

char *hasmntopt(const struct mntent *mnt, const char *opt) {
    if (!mnt->mnt_opts)
        return NULL;
    size_t optlen = strlen(opt);
    char *p = mnt->mnt_opts;
    while ((p = strstr(p, opt))) {
        if ((p == mnt->mnt_opts || p[-1] == ',') &&
            (p[optlen] == '\0' || p[optlen] == ',' || p[optlen] == '='))
            return p;
        p++;
    }
    return NULL;
}
