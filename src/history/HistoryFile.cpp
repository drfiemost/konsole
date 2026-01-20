
// Own
#include "HistoryFile.h"

// Konsole
#include "KonsoleSettings.h"

// System
#include <cerrno>
#include <unistd.h>

// KDE
#include <KStandardDirs>
#include <KDebug>

using namespace Konsole;

// History File ///////////////////////////////////////////
HistoryFile::HistoryFile()
    : _length(0),
      _fileMap(nullptr),
      _readWriteBalance(0)
{
    const QString tmpFormat = KStandardDirs::locateLocal("tmp", QString())
                              + QLatin1String("konsole-XXXXXX.history");
    _tmpFile.setFileTemplate(tmpFormat);
    if (_tmpFile.open()) {
        _tmpFile.setAutoRemove(true);
    }
}

HistoryFile::~HistoryFile()
{
    if (_fileMap)
        unmap();
}

//TODO:  Mapping the entire file in will cause problems if the history file becomes exceedingly large,
//(ie. larger than available memory).  HistoryFile::map() should only map in sections of the file at a time,
//to avoid this.
void HistoryFile::map()
{
    Q_ASSERT(_fileMap == nullptr);

    if (_tmpFile.flush()) {
        Q_ASSERT(_tmpFile.size() >= _length);
        _fileMap = _tmpFile.map(0, _length);
    }

    //if mmap'ing fails, fall back to the read-lseek combination
    if (_fileMap == nullptr) {
        _readWriteBalance = 0;
        kWarning() << "mmap'ing history failed.  errno = " << errno;
    }
}

void HistoryFile::unmap()
{
    Q_ASSERT(_fileMap != nullptr);

    if (_tmpFile.unmap(_fileMap))
        _fileMap = 0;

    Q_ASSERT(_fileMap == nullptr);
}

void HistoryFile::add(const char* buffer, qint64 count)
{
    if (_fileMap)
        unmap();

    if (_readWriteBalance < INT_MAX)
        _readWriteBalance++;

    qint64 rc = 0;

    if (!_tmpFile.seek(_length)) {
        perror("HistoryFile::add.seek");
        return;
    }
    rc = _tmpFile.write(buffer, count);
    if (rc < 0) {
        perror("HistoryFile::add.write");
        return;
    }
    _length += rc;
}

void HistoryFile::get(char* buffer, qint64 size, qint64 loc)
{
    if (loc < 0 || size < 0 || loc + size > _length) {
        fprintf(stderr, "getHist(...,%lld,%lld): invalid args.\n", size, loc);
        return;
    }

    //count number of get() calls vs. number of add() calls.
    //If there are many more get() calls compared with add()
    //calls (decided by using MAP_THRESHOLD) then mmap the log
    //file to improve performance.
    if (_readWriteBalance > INT_MIN)
        _readWriteBalance--;
    if (!_fileMap && _readWriteBalance < MAP_THRESHOLD)
        map();

    if (_fileMap) {
        std::memcpy(buffer, _fileMap + loc, size);
    } else {
        qint64 rc = 0;

        if (!_tmpFile.seek(loc)) {
            perror("HistoryFile::get.seek");
            return;
        }
        rc = _tmpFile.read(buffer, size);
        if (rc < 0) {
            perror("HistoryFile::get.read");
            return;
        }
    }
}

qint64 HistoryFile::len() const
{
    return _length;
}
