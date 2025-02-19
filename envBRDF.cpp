#include <getopt.h>
#include <tbb/parallel_for.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

#include "Math"

typedef unsigned char ubyte;
typedef unsigned int uint;

static Vec3f* cacheImportanceSampleGGX = 0;

inline void convertVec2ToUintsetRGB(ubyte* ptr, const Vec2f& val) {
    unsigned int A = uint(val[0] * 65535 + 0.5);  // + 0.5 I guess to avoid 0
    unsigned int B = uint(val[1] * 65535 + 0.5);  //

    ubyte v1 = (ubyte)(A >> 8 & 0xFF);
    ubyte v0 = (ubyte)A & 0xFF;

    ubyte v3 = (ubyte)(B >> 8 & 0xFF);
    ubyte v2 = (ubyte)B & 0xFF;

    ptr[0] = v0;
    ptr[1] = v1;

    ptr[2] = v2;
    ptr[3] = v3;

#if 0  // for debug
       // to experiment output
       // simluate unpacking

    double a = (ptr[0] + ptr[1]*(65280.0/255.0))/65535.0;
    double b = (ptr[2] + ptr[3]*(65280.0/255.0))/65535.0;

    double aDiff = fabs( a - val[0] );
    double bDiff = fabs( b - val[1] );

    if ( aDiff > 1e-4 )
        std::cerr << "something wrong in the lut encoding, error A " << aDiff << std::endl;

    if ( bDiff > 1e-4 )
        std::cerr << "something wrong in the lut encoding, error B " << bDiff << std::endl;
#endif
}

struct Worker {
    uint _size;
    uint _numSamples;
    Vec2f* _lut;
    float* _dataFace;

    Worker(uint numSamples, uint size, Vec2f* lut)
        : _numSamples(numSamples), _size(size), _lut(lut) {}

    // w is either Ln or Vn
    float G1_Schlick(float ndw, float k) const {
        // One generic factor of the geometry function divided by ndw
        // NB : We should have k > 0
        return 1.0 / (ndw * (1.0 - k) + k);
    }

    float G_Schlick(float ndv, float ndl, float k) const {
        // Schlick with Smith-like choice of k
        // cf
        // http://blog.selfshadow.com/publications/s2013-shading-course/karis/s2013_pbs_epic_notes_v2.pdf
        // p3
        float G1_ndl = G1_Schlick(ndl, k);
        float G1_ndv = G1_Schlick(ndv, k);
        return ndv * ndl * G1_ndl * G1_ndv;
    }

    // http://blog.selfshadow.com/publications/s2013-shading-course/karis/s2013_pbs_epic_notes_v2.pdf
    // page 7
    // this is the integrate function used to build the LUT
    Vec2f integrateBRDF(float roughnessLinear, float NoV, const int numSamples,
                        uint roughnessIndex) const {
        Vec3f V;
        V[0] = sqrt(1.0 - NoV * NoV);  // sin V.y = 0;
        V[1] = 0.0;
        V[2] = NoV;  // cos

        Vec3f N = Vec3f(0, 0, 1);  // dont know where the normal comes from
        // but if the view vector is generated from NoV then the normal should
        // be fixed and 0,0,1

        double A = 0.0;
        double B = 0.0;

        Vec3f UpVector = fabs(N[2]) < 0.999 ? Vec3f(0, 0, 1) : Vec3f(1, 0, 0);
        Vec3f TangentX = normalize(cross(UpVector, N));
        Vec3f TangentY = normalize(cross(N, TangentX));

        float roughness = roughnessLinear * roughnessLinear;
        float m = roughness;
        float k = m * 0.5;

        const Vec3f* cacheLineSampleGGX =
            &cacheImportanceSampleGGX[roughnessIndex * numSamples];
        Vec3f H, L;
        for (int i = 0; i < numSamples; i++) {
            // sample in local space
            // Vec3f H = importanceSampleGGX( i, numSamples, roughnessLinear);
            const Vec3f& localH = cacheLineSampleGGX[i];
            // sample in worldspace
            H = TangentX * localH[0] + TangentY * localH[1] + N * localH[2];

            L = H * (dot(V, H) * 2.0) - V;

            float NoL = saturate(L[2]);
            float NoH = saturate(H[2]);
            float VoH = saturate(V * H);

            if (NoL > 0.0) {
                float G = G_Schlick(NoV, NoL, k);
                float G_Vis = G * VoH / (NoH * NoV);

                float Fc = pow(1.0 - VoH, 5);
                A += (1.0 - Fc) * G_Vis;
                B += Fc * G_Vis;
            }
        }
        A /= numSamples;
        B /= numSamples;

        return Vec2f(clampTo(A, 0.0, 1.0), clampTo(B, 0.0, 1.0));
    }

    // https://google.github.io/filament/Filament.html#toc5.3.4.7
    Vec2f integrateBRDFMultiscatter(float roughnessLinear, float NoV, const int numSamples,
                        uint roughnessIndex) const {
        Vec3f V;
        V[0] = sqrt(1.0 - NoV * NoV);  // sin V.y = 0;
        V[1] = 0.0;
        V[2] = NoV;  // cos

        Vec3f N = Vec3f(0, 0, 1);  // dont know where the normal comes from
        // but if the view vector is generated from NoV then the normal should
        // be fixed and 0,0,1

        double A = 0.0;
        double B = 0.0;

        Vec3f UpVector = fabs(N[2]) < 0.999 ? Vec3f(0, 0, 1) : Vec3f(1, 0, 0);
        Vec3f TangentX = normalize(cross(UpVector, N));
        Vec3f TangentY = normalize(cross(N, TangentX));

        float roughness = roughnessLinear * roughnessLinear;
        float m = roughness;
        float k = m * 0.5;

        const Vec3f* cacheLineSampleGGX =
            &cacheImportanceSampleGGX[roughnessIndex * numSamples];
        Vec3f H, L;
        for (int i = 0; i < numSamples; i++) {
            // sample in local space
            // Vec3f H = importanceSampleGGX( i, numSamples, roughnessLinear);
            const Vec3f& localH = cacheLineSampleGGX[i];
            // sample in worldspace
            H = TangentX * localH[0] + TangentY * localH[1] + N * localH[2];

            L = H * (dot(V, H) * 2.0) - V;

            float NoL = saturate(L[2]);
            float NoH = saturate(H[2]);
            float VoH = saturate(V * H);

            if (NoL > 0.0) {
                float G = G_Schlick(NoV, NoL, k);
                float G_Vis = G * VoH / (NoH * NoV);

                float Fc = pow(1.0 - VoH, 5);
                A += Fc * G_Vis;
                B += G_Vis;
            }
        }
        A /= numSamples;
        B /= numSamples;

        return Vec2f(clampTo(A, 0.0, 1.0), clampTo(B, 0.0, 1.0));
    }

    void operator()(const tbb::blocked_range<uint>& r) const {
        float step = 1.0 / float(_size);

        for (uint y = r.begin(); y != r.end(); ++y) {
            float roughnessLinear = step * (y + 0.5);

            for (uint x = 0; x < _size; x++) {
                float NoV = step * (x + 0.5);

                Vec2f values =
                    integrateBRDFMultiscatter(roughnessLinear, NoV, _numSamples, y);

#if 0
                // Vec2d values2 = integrateBRDF( roughness, NoV, numSamples2);

                double diff0 = fabs(values[0]-values2[0]);
                double diff1 = fabs(values[1]-values2[1]);
                // if ( diff0 > 1e-4 || diff1 > 1e-4 ) {
                //     std::cout << "diff0 " << diff0 << " diff1 " << diff1 << std::endl;
                // }
                maxError = std::max( diff1, maxError);
                maxError = std::max( diff0, maxError);
#endif
                _lut[x + y * _size] = values;
            }
        }
    }
};

struct WorkerPrepareCache {
    uint _size;
    uint _numSamples;

    WorkerPrepareCache(uint numSamples, uint size)
        : _numSamples(numSamples), _size(size) {}

    void operator()(const tbb::blocked_range<uint>& r) const {
        float step = 1.0 / float(_size);

        for (uint y = r.begin(); y != r.end(); ++y) {
            float roughnessLinear = step * (y + 0.5);
            uint cacheLineIndex = y;

            for (uint i = 0; i < _numSamples; i++) {
                cacheImportanceSampleGGX[cacheLineIndex * _numSamples + i] =
                    importanceSampleGGX(i, _numSamples, roughnessLinear);
            }
        }
    }
};

struct RougnessNoVLUT {
    int _size;
    Vec2f* _lut;
    double _maxValue;
    uint _nbSamples;

    RougnessNoVLUT(int size, uint samples = 1024) {
        _size = size;
        _lut = new Vec2f[size * size];
        _maxValue = 0.0;
        _nbSamples = samples;
    }

    void prepareCacheGGX(uint numSamples, uint size) {
        cacheImportanceSampleGGX = new Vec3f[numSamples * size];
        parallel_for(tbb::blocked_range<uint>(0, _size),
                     WorkerPrepareCache(numSamples, size));
    }

    // LUT generation main entry point
    // from
    // http://blog.selfshadow.com/publications/s2013-shading-course/karis/s2013_pbs_epic_notes_v2.pdf
    void processRoughnessNoVLut(const std::string& filename) {
        float step = 1.0 / float(_size);

        uint numSamples = pow(2, uint(floor(log2(_nbSamples))));

        prepareCacheGGX(numSamples, _size);

        parallel_for(tbb::blocked_range<uint>(0, _size),
                     Worker(numSamples, _size, _lut));

        writeImage(filename.c_str(), _size, _size, _lut);
    }

    int writeImage(const char* filename, int width, int height, Vec2f* buffer) {
        ubyte* data = new ubyte[width * height * 4];
        for (int i = 0; i < width * height; i++) {
            convertVec2ToUintsetRGB(data + i * 4, buffer[i]);
        }

        FILE* file = fopen(filename, "wb");
        fwrite(data, width * height * 4, 1, file);
        delete[] data;

        return 0;
    }
};

static int usage(const std::string& name) {
    std::cerr << "Usage: " << name << " [-s size] out.raw" << std::endl;
    return 1;
}

int main(int argc, char* argv[]) {
    int size = 0;
    uint samples = 1024;
    int c;

    while ((c = getopt(argc, argv, "s:n:")) != -1) switch (c) {
            case 's':
                size = atof(optarg);
                break;
            case 'n':
                samples = atof(optarg);
                break;

            default:
                return usage(argv[0]);
        }

    std::string input, output;

    if (optind < argc) {
        // generate roughness nov
        output = std::string(argv[optind]);

        if (!size) size = 256;

        RougnessNoVLUT lut(size);
        lut.processRoughnessNoVLut(output);

    } else {
        return usage(argv[0]);
    }

    return 0;
}
