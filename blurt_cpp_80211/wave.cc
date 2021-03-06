#include "wave.h"
#include "chunk.h"
#include <fstream>
#include <string.h>

#if !__EXCEPTIONS
#define throw std::cerr <<
#endif

typedef struct {
    uint16_t wFormatTag;
    uint16_t wChannels;
    uint32_t dwFrameRate;
    uint32_t dwAvgBytesPerSec;
    uint16_t wBlockAlign;
    uint16_t wBitsPerSample;
} WaveFormatEx;

const static int WAVE_FORMAT_PCM = 0x0001;

class Wave_read {
private:
    size_t framesize;
    Chunk file_chunk, *data_chunk;

    void read_fmt_chunk(Chunk &chunk) {
        WaveFormatEx fmt;
        chunk.read(&fmt, 14);
        nchannels = fmt.wChannels;
        framerate = fmt.dwFrameRate;
        if (fmt.wFormatTag == WAVE_FORMAT_PCM) {
            sampwidth = 0;
            chunk.read(&sampwidth, 2);
            sampwidth = (sampwidth + 7) / 8;
        } else {
            throw "unknown format";
        }
        framesize = nchannels * sampwidth;
        comptype = "NONE";
        compname = "not compressed";
    }

public:
    std::string comptype, compname;
    size_t nframes, nchannels, framerate, sampwidth;

    Wave_read(std::ifstream &file) : file_chunk(file)
    {
        bool fmt_chunk_read = false, data_chunk_read = false;
        if (file_chunk.id.compare("RIFF") != 0)
            throw "file does not start with RIFF id";
        std::string format;
        format.resize(4);
        file_chunk.read(&format[0], 4);
        if (format.compare("WAVE") != 0)
            throw "not a WAVE file";
        file_chunk.parseSubchunks();
        for (size_t i=0; i<file_chunk.subchunks.size(); i++) {
            Chunk &chunk = file_chunk.subchunks[i];
            if (chunk.id.compare("fmt ") == 0) {
                read_fmt_chunk(chunk);
                fmt_chunk_read = true;
            } else if (chunk.id.compare("data") == 0) {
                if (!fmt_chunk_read)
                    throw "data chunk before fmt chunk";
                data_chunk = &chunk;
                data_chunk_read = true;
                nframes = chunk.size / framesize;
                break;
            }
        }
        if (!fmt_chunk_read || !data_chunk_read)
            throw "fmt chunk and/or data chunk missing";
    }

    ~Wave_read() {
        close();
    }

    void close() {
        file_chunk.close();
    }

    void readframes(void *buffer, size_t nframes_) {
        data_chunk->read(buffer, nframes_ * framesize);
    }
};

class Wave_write {
private:
    bool headerwritten;
    Chunk file_chunk, *data_chunk;

public:
    size_t nframes, nchannels, framerate, sampwidth;
    std::string comptype, compname;

    Wave_write(std::ofstream &file) :
        file_chunk(file, "RIFF"), nframes(0), nchannels(0), framerate(0), sampwidth(0),
        comptype("NONE"), compname("not compressed")
    {
        headerwritten = false;
        file_chunk.write((char *)"WAVE", 4);
    }

    ~Wave_write() {
        close();
    }

    void setnchannels(size_t nchannels_) {
        if (headerwritten) throw "cannot change parameters after starting to write";
        if (nchannels_ < 1) throw "bad # of channels";
        this->nchannels = nchannels_;
    }

    void setsampwidth(size_t sampwidth_) {
        if (headerwritten) throw "cannot change parameters after starting to write";
        if (sampwidth_ < 1 || sampwidth_ > 4) throw "bad sample width";
        this->sampwidth = sampwidth_;
    }

    void setframerate(size_t framerate_) {
        if (headerwritten) throw "cannot change parameters after starting to write";
        if (framerate_ <= 0) throw "bad frame rate";
        this->framerate = framerate_;
    }

    void writeframes(void *data, size_t nframes_) {
        size_t bytes = nframes_ * sampwidth * nchannels;
        _ensure_header_written();
        data_chunk->write(data, bytes);
    }

    void close() {
        _ensure_header_written();
        file_chunk.close();
    }

    void _ensure_header_written() {
        if (!headerwritten) {
            if (!nchannels) throw "# channels not specified";
            if (!sampwidth) throw "sample width not specified";
            if (!framerate) throw "sampling rate not specified";

            Chunk &fmt_chunk = file_chunk.addSubchunk("fmt ");

            WaveFormatEx fmt;
            fmt.wFormatTag = WAVE_FORMAT_PCM;
            fmt.wChannels = (uint16_t)nchannels;
            fmt.dwFrameRate = (uint32_t)framerate;
            fmt.dwAvgBytesPerSec = (uint32_t)(nchannels * framerate * sampwidth);
            fmt.wBlockAlign = (uint16_t)(nchannels * sampwidth);
            fmt.wBitsPerSample = (uint16_t)(sampwidth * 8);
            fmt_chunk.write(&fmt, 16);

            data_chunk = &file_chunk.addSubchunk("data");
            headerwritten = true;
        }
    }
};

bool readwave(const std::string &filename, std::vector<float> &output, float &Fs) {
    std::ifstream file(filename.c_str(), std::fstream::in | std::fstream::binary);
    if (!file.is_open()) {
        output.clear();
        Fs = 0;
        return false;
    }
    Wave_read f(file);
    Fs = f.framerate;
    size_t nchannels = f.nchannels;
    size_t nframes = f.nframes;
    size_t sampwidth = f.sampwidth;
    output.resize(nframes);
    char *readbuf = new char[sampwidth*nchannels];
    float scale = 1.f/nchannels;
    for (size_t i=0; i<nframes; i++) {
        float acc = 0;
        f.readframes(readbuf, 1);
        for (size_t j=0; j<nchannels; j++) {
            int32_t sample = 0;
            memcpy(&sample, &readbuf[j*sampwidth], sampwidth);
            if (sampwidth == 1)
                sample -= 0x80;
            else if (sampwidth == 2)
                sample = (int32_t)(int16_t)sample;
            else if (sampwidth == 3)
                sample = ((sample << 8) >> 8);
            //sample += ~(((sample >> ((sampwidth<<3) -1))<<(sampwidth<<3))-1);
            acc += sample * scale;
        }
        output[i] = acc;
    }
    delete [] readbuf;
    return true;
}

bool writewave(const std::string &filename, const std::vector<float> &input, size_t Fs, size_t sampwidth, size_t nchannels) {
    std::ofstream file(filename.c_str(), std::fstream::out | std::fstream::binary);
    if (!file.is_open()) {
        return false;
    }
    size_t nframes = input.size()/nchannels;
    float scale = 1ull<<(8*sampwidth);
    std::vector<char> quantized_input(input.size()*sampwidth);
    for (size_t i=0; i<input.size(); i++) {
        float clipped_input = input[i];
        clipped_input = (clipped_input > 1.f) ? 1.f : clipped_input;
        clipped_input = (clipped_input < -1.f) ? -1.f : clipped_input;
        int32_t sample = int32_t(clipped_input * scale + 1) >> 1; // proper rounding
        if (sampwidth == 1)
            sample += 0x80;
        memcpy(&quantized_input[i*sampwidth], &sample, sampwidth);
    }
    Wave_write f(file);
    f.setnchannels(nchannels);
    f.setsampwidth(sampwidth);
    f.setframerate(Fs);
    f.writeframes(&quantized_input[0], nframes);
    f.close();
    file.flush();
    file.close();
    return true;
}
