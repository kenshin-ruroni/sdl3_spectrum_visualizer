#include <thread>
#include <chrono>
#include <functional> // Requis pour std::hash
#include <unordered_map>
#include <stdint.h>

#define DR_WAV_IMPLEMENTATION
#define DR_FLAC_IMPLEMENTATION
#define DR_MP3_IMPLEMENTATION
#include "./dr_libs/dr_wav.h"
#include "./dr_libs/dr_flac.h"
#include "./dr_libs/dr_mp3.h"

#include <vector>
#include <deque>

#include "miniaudio.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_sdl3.h"

#include  "imgui_impl_sdlrenderer3.h"

#include "sse2_fft.h"

#include "spectrogram_renderer.h"
#include "imfilebrowser.h"

static auto now()
{
        return  std::chrono::high_resolution_clock::now();
}
static double now_to_seconds()
{
       return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::time_point_cast<std::chrono::nanoseconds>( now() ).time_since_epoch()).count() * 1.0e-9;
}

static inline double elapsed_time_in_seconds(std::chrono::time_point<std::chrono::high_resolution_clock> start,
                                                 std::chrono::time_point<std::chrono::high_resolution_clock> end)
    {
        return  abs(std::chrono::duration_cast<std::chrono::microseconds>( end - start ).count())*1e-6;
    }

static std::string duration_to_hhmmss(double duration)
    {

        duration = std::fmod(duration,24.*3600.);
        size_t hh = duration/3600;
        duration = std::fmod(duration,3600.);
        size_t mm = duration/60;
        duration = std::fmod(duration,60.);
        size_t ss = duration;
        duration -= (double)ss;
        duration = round(duration*100000.);
        std::string r = std::to_string(hh)+":"+std::to_string(mm)+":"+std::to_string(ss)+"."+std::to_string(duration);
        return r;
    }


inline bool load_wav_file(std::string &path,std::vector<float> *interleaved_samples, uint *channels, uint *sample_rate)
{

    drwav_uint64 totalPCMFrameCount;

    float* pSampleData = drwav_open_file_and_read_pcm_frames_f32(path.c_str(), channels, sample_rate, &totalPCMFrameCount, NULL);
    
    if (pSampleData == NULL)
    {
        // Error opening and reading WAV file.
        std::string msg = "an error occurred while loading file '"; msg+=path.c_str();msg+="'.";
        printf("%s \n", msg.c_str());
        return false;
    }

    // build node data
    //  copy samples to node data structure
    interleaved_samples->resize(totalPCMFrameCount* *channels);
    memcpy(interleaved_samples->data(),pSampleData,totalPCMFrameCount* *channels * sizeof(float));
    return true;
}

static bool load_flac_file(std::string &path,std::vector<float> *interleaved_samples, uint *channels, uint *sample_rate)
{

    drwav_uint64 totalPCMFrameCount;
    float* pSampleData = drflac_open_file_and_read_pcm_frames_f32(path.c_str(), channels, sample_rate, &totalPCMFrameCount, NULL);
    if (pSampleData == NULL)
    {
        // Error opening and reading WAV file.
        std::string msg = "an error occurred while loading file '"; msg+=path.c_str();msg+="'.";
        printf("%s \n", msg.c_str());
        return false;
    }

    // build node data
    //  copy samples to node data structure
    interleaved_samples->resize(totalPCMFrameCount * *channels);
    memcpy(interleaved_samples->data(),pSampleData,totalPCMFrameCount* *channels*sizeof(float));
    drwav_free(pSampleData, NULL); // free memory
    return true;
}

static inline bool load_mp3_file(std::string &path,std::vector<float> *interleaved_samples, uint *channels, uint *sample_rate)
    {

        drmp3_uint64 totalPCMFrameCount = 0;
        drmp3_config config;



        float* pSampleData = drmp3_open_file_and_read_pcm_frames_f32(path.c_str(),&config, &totalPCMFrameCount, NULL);
        if (pSampleData == NULL)
        {
            std::string msg = "an error occurred while loading file '"; msg+=path.c_str();msg+="'.";
            printf("%s \n", msg.c_str());
            return false;
        }
        *channels = config.channels;
        *sample_rate = config.sampleRate;
        // build node data
        //  copy samples to node data structure
        interleaved_samples->resize(totalPCMFrameCount*config.channels);
        memcpy(interleaved_samples->data(),pSampleData,totalPCMFrameCount* *channels *sizeof(float));
        drwav_free(pSampleData, NULL); // free memory
        return true;
    }


        // Wave, audio wave data
        typedef struct Wave
        {
            unsigned int frameCount;    // Total number of frames (considering channels)
            unsigned int sampleRate;    // Frequency (samples per second)
            unsigned int sampleSize;    // Bit depth (bits per sample): 8, 16, 32 (24 not supported)
            unsigned int channels;      // Number of channels (1-mono, 2-stereo, ...)
            void *data;                 // Buffer data pointer
        } Wave;

        // Convert wave data to desired format
        inline void WaveFormat(Wave *wave, int sampleRate, int sampleSize, int channels)
        {
            ma_format formatIn = ((wave->sampleSize == 8)? ma_format_u8 : ((wave->sampleSize == 16)? ma_format_s16 : ma_format_f32));
            ma_format formatOut = ((sampleSize == 8)? ma_format_u8 : ((sampleSize == 16)? ma_format_s16 : ma_format_f32));

            ma_uint32 frameCountIn = wave->frameCount;
            ma_uint32 frameCount = (ma_uint32)ma_convert_frames(NULL, 0, formatOut, channels, sampleRate, NULL, frameCountIn, formatIn, wave->channels, wave->sampleRate);

            if (frameCount == 0)
            {
                printf( "WAVE: Failed to get frame count for format conversion" );
                return;
            }

            void *data = malloc(frameCount*channels*(sampleSize/8));

            frameCount = (ma_uint32)ma_convert_frames(data, frameCount, formatOut, channels, sampleRate, wave->data, frameCountIn, formatIn, wave->channels, wave->sampleRate);
            if (frameCount == 0)
            {
                free(wave->data);
                wave->data = nullptr;
                printf( "WAVE: Failed format conversion");
                return;
            }

            wave->frameCount = frameCount;
            wave->sampleSize = sampleSize;
            wave->sampleRate = sampleRate;
            wave->channels = channels;

            free(wave->data);

            wave->data = data;
        }



        static inline bool ma_save_file_data(const char *fileName, void *data, int dataSize)
        {
            if (fileName != nullptr)
            {
                FILE *file = fopen(fileName, "wb");

                if (file != nullptr)
                {
                    unsigned int count = (unsigned int)fwrite(data, sizeof(unsigned char), dataSize, file);

                    if (count == 0) printf( "FILEIO: [%s] Failed to write file", fileName);
                    else if (count != dataSize) printf( "FILEIO: [%s] File partially written", fileName);
                    else printf( "FILEIO: [%s] File saved successfully", fileName);

                    fclose(file);
                }
                else
                {
                    printf( "FILEIO: [%s] Failed to open file", fileName);
                    return false;
                }
            }
            else
            {
                printf( "FILEIO: File name provided is not valid");
                return false;
            }

            return true;
        }

        // Export wave data to file
        static inline bool export_wave(Wave wave, const char *fileName)
        {
            bool success = false;

            drwav wav = { 0 };
            drwav_data_format format;
            format.container = drwav_container_riff;
            if (wave.sampleSize == 32) format.format = DR_WAVE_FORMAT_IEEE_FLOAT;
            else format.format = DR_WAVE_FORMAT_PCM;
            format.channels = wave.channels;
            format.sampleRate = wave.sampleRate;
            format.bitsPerSample = wave.sampleSize;

            void *fileData = NULL;
            size_t fileDataSize = 0;
            success = drwav_init_memory_write(&wav, &fileData, &fileDataSize, &format, NULL);
            if (success) success = (int)drwav_write_pcm_frames(&wav, wave.frameCount, wave.data);
            drwav_result result = drwav_uninit(&wav);

            if (result == DRWAV_SUCCESS) success = ma_save_file_data(fileName, (unsigned char *)fileData, (unsigned int)fileDataSize);

            drwav_free(fileData, NULL);

                return success;
        }
        static inline bool ma_read_from_wav_file(const char *file_name, std::vector<float> *interleaved_samples, size_t *channels, size_t *sample_rate)
        {
            Wave wave = { 0 };

            // Loading file to memory
          //  int dataSize = 0;
          //  unsigned char *file_data = ma_load_file_data(file_name.toStdString().c_str(), &dataSize);

            // Loading wave from memory data
            drwav wav = { 0 };

            bool success = drwav_init_file(&wav, file_name, NULL);

            if (success)
            {
                wave.frameCount = (unsigned int)wav.totalPCMFrameCount;
                wave.sampleRate = wav.sampleRate;
                wave.sampleSize = wav.bitsPerSample;
                wave.channels = wav.channels;
                std::vector<short> samples; samples.resize(wave.frameCount* wave.channels );
                wave.data = samples.data();
                *sample_rate = wave.sampleRate;
                *channels = wave.channels;
                // NOTE: We are forcing conversion to 16bit sample size on reading

                size_t samples_size = wave.frameCount;
                if (wave.sampleSize == 8)
                {
                    drwav_read_pcm_frames(&wav,samples_size,wave.data);
                }
                drwav_uint64 frames_read = drwav_read_pcm_frames_s16(&wav, wave.frameCount, (drwav_int16 *) wave.data);
                if ( frames_read != samples_size)
                {
                    printf("file corrupted. aborted.");
                    return false;
                }
                interleaved_samples->resize(samples_size * *channels);

                short * frame = (short *)wave.data;
                for (size_t i = 0; i < samples_size ;  i += *channels)
                {
                    /*  if (wave.sampleSize == 8)
                    {
                        for (size_t k = 0; k < *channels; k++)
                        {
                            *(interleaved_samples->data() + *channels * i + k)  = (T)(((uint8_t *)(wave.data))[i + k] - 128)/128.0f;

                        }
                    }
                    else if (wave.sampleSize == 16) */
                    // {
                    for (size_t k = 0; k < *channels; k++)
                    {
                        *(interleaved_samples->data() + *channels * i + k) = (float)(frame[i + k])/32768.0f;

                      //  printf(" %i  %i   %zu  %f \n",i+k,frame[i + k], *channels * i + k , (*interleaved_samples)[*channels * i + k]);

                    }
                    // }
                    /*
                    else if (wave.sampleSize == 32)
                    {
                        for (size_t k = 0; k < *channels; k++)
                        {
                            *(interleaved_samples->data() + *channels * i + k) = ((T *)wave.data)[i +k];
                        }
                    }*/
                }

            }
            else printf("WAVE: Failed to load WAV data");

            return  success  = drwav_uninit(&wav);


          //  free(file_data);

        }

    // Tampons pour la FFT courante
    std::vector<float> currentLeft = std::vector<float>(FFT_SIZE, 0.0f);
    std::vector<float> currentRight = std::vector<float>(FFT_SIZE, 0.0f);
    std::vector<std::complex<float>> fftLeft = std::vector<std::complex<float>>(FFT_SIZE); std::vector<std::complex<float>> fftRight = std::vector<std::complex<float>>(FFT_SIZE);

    size_t paneHeight = WINDOW_HEIGHT / 2;

    // Fonction FFT Cooley-Tukey (identique)
inline void fft(std::vector<std::complex<float>>* a) {
    int n = a->size();
    if (n <= 1) return;
    std::vector<std::complex<float>> a0(n / 2), a1(n / 2);
    for (int i = 0; 2 * i < n; i++) {
        a0[i] = (*a)[2 * i];
        a1[i] = (*a)[2 * i + 1];
    }
    fft(&a0); fft(&a1);
    float angle = 2 * M_PI / n;
    std::complex<float> w(1), wn(std::cos(angle), -std::sin(angle));
    for (int i = 0; 2 * i < n; i++) {
        (*a)[i] = a0[i] + w * a1[i];
        (*a)[i + n / 2] = a0[i] - w * a1[i];
        w *= wn;
    }
}

constexpr float threshold = 1.;

    alignas(64) std::atomic<bool> playback = false; 
    alignas(64) std::atomic<bool> stop_playback_requested = false;
    alignas(64) std::atomic<size_t> samples_cursor = 0, next_cursor = 0;
    alignas(64) std::atomic<float> gain_db = 0.f;
    alignas(64) std::atomic<float> current_playing_time = 0.;
    alignas(64) std::atomic<float> gain = std::pow(10.f,gain_db/20.f);

        alignas(64) std::atomic<uint> channels = 2; 
    alignas(64) std::atomic<uint> sample_rate = 44100;
    alignas(64) std::atomic<bool> file_loaded = false;
    alignas(64) std::atomic<bool> update_playback_stream = true;
    alignas(64) std::atomic<bool> file_loading = false;
    alignas(64) std::atomic<bool> error_file_loading = false;
    std::string error_file_loading_msg;

std::vector<uint32_t> pixelBuffer;
    // Ajoute une nouvelle colonne de fréquences calculées et décale le reste de l'image
    void addFFTFrame(const std::vector<float> *magnitudes) {
        // 1. Décaler tous les pixels de la texture d'un pixel vers la gauche
        for (int y = 0; y < WINDOW_HEIGHT; ++y) {
            std::memmove(&pixelBuffer[y * WINDOW_WIDTH], &pixelBuffer[y * WINDOW_WIDTH + 1], (WINDOW_WIDTH - 1) * sizeof(uint32_t));
        }

        // 2. Dessiner la nouvelle colonne tout à droite (X = WINDOW_WIDTH - 1)
        // L'axe Y représente les fréquences (Basses en bas, Hautes en haut)
        for (int y = 0; y < WINDOW_HEIGHT; ++y) {
            // Mapper la hauteur de l'écran sur la moitié utile de la FFT (frequences positives)
            uint32_t fftBin = (WINDOW_HEIGHT - 1 - y) * (FFT_SIZE / 2) / WINDOW_HEIGHT;

            size_t index = std::clamp(fftBin, (uint32_t)0, FFT_SIZE / 2 - 1);
            float mag = (*magnitudes)[index];

            // Normalisation de l'intensité lumineuse (application d'une échelle logarithmique)
            float intensity = std::clamp(20.0f * std::log10(mag + 1.0f) * 10.0f, 0.0f, 255.0f);
            uint8_t colorVal = static_cast<uint8_t>(intensity);

            // Génération d'une palette de couleur (Ex: Dégradé de Vert)
            uint32_t rgbaColor = (0x00 << 24) | (colorVal << 16) | (0x00 << 8) | 0xFF; // RGBA
            
            pixelBuffer[y * WINDOW_WIDTH + (WINDOW_WIDTH - 1)] = rgbaColor;
        }
        
    }


        std::vector<float> buffer(2 * FFT_SIZE);  

std::vector<std::complex<float>> fftData(FFT_SIZE);
std::vector<float> spectrogram_magnitudes = std::vector<float> (FFT_SIZE/2, 0.0f);
    inline void process_spectrogram(size_t next_cursor,size_t samples_cursor,int minimum_audio,std::vector<float> *samples,int pitch, SDL_AudioStream* stream,SDL_Renderer *renderer,SDL_Texture*  texture ,float *buffer)
    {

        if (SDL_GetAudioStreamQueued(stream) < minimum_audio)
            {
                // this will feed 1024 samples each frame until we get to our maximum. 
                // generate samples from grooves 
                next_cursor = std::min(samples->size() - 1, samples_cursor + SDL_arraysize(buffer)) ;
                memcpy(buffer, (const void *)(samples->data()+samples_cursor), (next_cursor - samples_cursor) * sizeof(float) );

                // feed the new data to the stream. It will queue at the end, and trickle out as the hardware needs more data. 
                SDL_PutAudioStreamData(stream, buffer, (next_cursor - samples_cursor) * sizeof(float) );
                samples_cursor = next_cursor;
                if ( samples_cursor >= samples->size() - 1 )
                {
                    return;
                }

            int samplesNeeded = SDL_arraysize(buffer);
                if (samplesNeeded >= FFT_SIZE) 
                { 

// Récupérer les données du flux
                    // Préparer les données pour la FFT complexe
                    
                    for (int i = 0; i < FFT_SIZE; ++i) 
                    {
                        // Optionnel mais recommandé : appliquer une fenêtre de Hamming ici pour éviter le "spectral leakage"
                        fftData[i] = std::complex<float>(buffer[i], 0.0f);
                    }
                    fft(&fftData);
                    // Extraction des amplitudes de la moitié positive du spectre

                    for (int i = 0; i < FFT_SIZE / 2; ++i) {
                        spectrogram_magnitudes[i] = std::abs(fftData[i]);
                    }

                    addFFTFrame(&spectrogram_magnitudes);

                    // Rendu graphique
                    SDL_SetRenderDrawColor(renderer, 50, 50, 50, 255);
                    SDL_RenderClear(renderer);
                    SDL_UpdateTexture(texture,nullptr,pixelBuffer.data(),pitch);
                    SDL_RenderTexture(renderer, texture, nullptr, nullptr);
                    SDL_RenderPresent(renderer);
                }
            }
        }

    constexpr float norm = 1.f/2.f;
    inline void process_spectrum_play_back(size_t paneHeight,int minimum_audio,  SDL_AudioStream* stream,SDL_Renderer *renderer, std::vector<float> *samples,std::vector<float> *buffer)
    {
            if (playback.load() == true && SDL_GetAudioStreamQueued(stream) < minimum_audio)
            {
                // this will feed 1024 samples each frame until we get to our maximum. 
                // generate samples from grooves 
                next_cursor.store( std::min(samples->size() - 1, samples_cursor.load() + buffer->size() ) );
                memcpy(buffer->data(), (const void *)(samples->data()+samples_cursor), (next_cursor - samples_cursor) * sizeof(float) );

                // feed the new data to the stream. It will queue at the end, and trickle out as the hardware needs more data. 
                SDL_PutAudioStreamData(stream, buffer->data(), (next_cursor - samples_cursor) * sizeof(float) );
                samples_cursor.store( next_cursor );
                current_playing_time = samples_cursor/sample_rate/channels;
                if ( samples_cursor >= samples->size() - 1 )
                {
                    playback.store(false);
                    return;
                }

            int samplesNeeded = buffer->size();
            if (samplesNeeded >= FFT_SIZE) 
            { 
                for (size_t  i = 0; i < FFT_SIZE; ++i) 
                {
                    currentLeft[i] = gain.load() * (*buffer)[i * 2];
                    currentRight[i] = gain.load() * (*buffer)[i * 2 + 1];
                }

                for (int i = 0; i < FFT_SIZE; ++i) {
                    float windowMultiplier = 0.5f * (1.0f - std::cos(2.0f * M_PI * i / (FFT_SIZE - 1)));
                    fftLeft[i] = std::complex<float>(currentLeft[i] * windowMultiplier, 0.0f);
                    fftRight[i] = std::complex<float>(currentRight[i] * windowMultiplier, 0.0f);
                }
                fft(&fftLeft);
                fft(&fftRight);
            }

        }

        SDL_SetRenderDrawColor(renderer, 10, 10, 10, 255);
            SDL_RenderClear(renderer);
            int usableBins = FFT_SIZE /2; // Fréquences utiles uniques
            float barWidth = static_cast<float>(WINDOW_WIDTH) / usableBins;
            for (int i = 0; i < usableBins; ++i) {
                // Calcul de l'amplitude (mise à l'échelle logarithmique visuelle)
                float magLeft = std::abs(fftLeft[i] *= 0.95) / std::sqrt(FFT_SIZE);
                float magRight = std::abs(fftRight[i] *= 0.95) / std::sqrt(FFT_SIZE);

                float normLeft = std::clamp(std::log1p(magLeft * 20.0f) *norm, 0.0f, 1.0f);
                float normRight = std::clamp(std::log1p(magRight * 20.0f) *norm , 0.0f, 1.0f);

                float x = static_cast<int>(i * barWidth);
                float w = std::max(1, static_cast<int>(barWidth));

                // --- Canal Gauche (Panneau du haut) ---
                int hLeft =  static_cast<int>(normLeft * (paneHeight - 20));
                SDL_FRect rectLeft{ (float)x, (float)(paneHeight - hLeft), (float)w, (float)hLeft };

                uint8_t r = static_cast<uint8_t>(threshold * normLeft * 255.);
                uint8_t g = static_cast<uint8_t>((1. - threshold * normLeft) * 255.);
                SDL_SetRenderDrawColor(renderer, r, 0, g, 255); // Cyan
                SDL_RenderFillRect(renderer, &rectLeft);

                // --- Canal Droit (Panneau du bas) ---
                int hRight =  static_cast<int>(normRight * (paneHeight - 20));
                r = static_cast<uint8_t>(threshold * normRight * 255.);
                g = static_cast<uint8_t>((1. - threshold * normRight) * 255.);
                SDL_FRect rectRight{ x, (float)(paneHeight ), w, (float)hRight };
                SDL_SetRenderDrawColor(renderer, r,0,g, 255); // Magenta
                SDL_RenderFillRect(renderer, &rectRight);
            }

            // Ligne de séparation médiane
           // SDL_SetRenderDrawColor(renderer, 255, 255, 255, 100);
           // SDL_RenderLine(renderer, 0, paneHeight, WINDOW_WIDTH, paneHeight);
    }

    size_t last_audio_cursor_stream = 0;
    alignas(64) std::atomic<size_t> audio_cursor_stream = 0;

    std::vector<float> tmp_buffer(4 * FFT_SIZE);
    inline void process_spectrum_capture_to_playback(SDL_Renderer* renderer, SDL_AudioStream* playback_stream , SDL_AudioStream* capture_stream, std::vector<float> *buffer)
    {
        // Rendu graphique
            SDL_SetRenderDrawColor(renderer, 10, 10, 10, 255);
            SDL_RenderClear(renderer);

            int buffer_size = SDL_GetAudioStreamAvailable( capture_stream );  // number of bytes the stream has accumulated so far.
            
            if ( playback.load() == true && buffer_size > 0)
            {
                std::vector<float> buff(buffer_size);
                size_t frame_size_in_bytes = sizeof(float) * 2;

                SDL_GetAudioStreamData(capture_stream,buff.data(),buffer_size);

                last_audio_cursor_stream = audio_cursor_stream;
                audio_cursor_stream = std::min( audio_cursor_stream + buffer_size, buffer->size()  - 1);

               memcpy(buffer->data() + last_audio_cursor_stream, buff.data(), (audio_cursor_stream-last_audio_cursor_stream) *sizeof(float) );
               
                int samplesbuffered = audio_cursor_stream;
                if (samplesbuffered == buffer->size()  - 1) 
                { 

                    audio_cursor_stream.store( audio_cursor_stream % buffer->size()  - 1 );


                    for (size_t  i = 0; i < FFT_SIZE; ++i) 
                    {
                        currentLeft[i] = (*buffer)[i * 2];
                        currentRight[i] = (*buffer)[i * 2 + 1];
                    }

                    for (int i = 0; i < FFT_SIZE; ++i) {
                        float windowMultiplier = 0.5f * (1.0f - std::cos(2.0f * M_PI * i / (FFT_SIZE - 1)));
                        fftLeft[i] = std::complex<float>(currentLeft[i] * windowMultiplier, 0.0f);
                        fftRight[i] = std::complex<float>(currentRight[i] * windowMultiplier, 0.0f);
                    }
                    fft(&fftLeft);
                    fft(&fftRight);

                    /*int usableBins = FFT_SIZE /2; // Fréquences utiles uniques
                    float barWidth = static_cast<float>(WINDOW_WIDTH) / usableBins;
                    for (int i = 0; i < usableBins; ++i) {
                        // Calcul de l'amplitude (mise à l'échelle logarithmique visuelle)
                        float magLeft = std::abs(fftLeft[i]) / std::sqrt(FFT_SIZE);
                        float magRight = std::abs(fftRight[i]) / std::sqrt(FFT_SIZE);

                        float normLeft = std::clamp(std::log1p(magLeft * 20.0f) *norm, 0.0f, 1.0f);
                        float normRight = std::clamp(std::log1p(magRight * 20.0f) *norm , 0.0f, 1.0f);

                        int x = static_cast<int>(i * barWidth);
                        int w = std::max(5, static_cast<int>(barWidth));

                        // --- Canal Gauche (Panneau du haut) ---
                        int hLeft =  static_cast<int>(normLeft * (paneHeight - 20));
                        SDL_FRect rectLeft{ (float)x, (float)(paneHeight - hLeft), (float)w, (float)hLeft };

                        uint8_t r = static_cast<uint8_t>(threshold * normLeft * 255.);
                        uint8_t g = static_cast<uint8_t>((1. - threshold * normLeft) * 255.);
                        SDL_SetRenderDrawColor(renderer, r, 0, g, 255); // Cyan
                        SDL_RenderFillRect(renderer, &rectLeft);

                        // --- Canal Droit (Panneau du bas) ---
                        int hRight =  static_cast<int>(normRight * (paneHeight - 20));
                        r = static_cast<uint8_t>(threshold * normRight * 255.);
                        g = static_cast<uint8_t>((1. - threshold * normRight) * 255.);
                        SDL_FRect rectRight{ (float)x, (float)(paneHeight ), (float)w, (float)hRight };
                        SDL_SetRenderDrawColor(renderer, r,0,g, 255); // Magenta
                        SDL_RenderFillRect(renderer, &rectRight);
                    }*/

                    
                }
                // playback
                SDL_PutAudioStreamData(playback_stream,buff.data(),buffer_size );
            }

            int usableBins = FFT_SIZE /2; // Fréquences utiles uniques
                    float barWidth = static_cast<float>(WINDOW_WIDTH) / usableBins;
                    for (int i = 0; i < usableBins; ++i) 
                    {
                        // Calcul de l'amplitude (mise à l'échelle logarithmique visuelle)
                        float magLeft = std::abs(fftLeft[i] *= 0.95) / std::sqrt(FFT_SIZE);
                        float magRight = std::abs(fftRight[i]*= 0.95) / std::sqrt(FFT_SIZE);

                        float normLeft = std::clamp(std::log1p(magLeft * 20.0f) *norm, 0.0f, 1.0f);
                        float normRight = std::clamp(std::log1p(magRight * 20.0f) *norm , 0.0f, 1.0f);

                        int x = static_cast<int>(i * barWidth);
                        int w = std::max(2, static_cast<int>(barWidth));

                        // --- Canal Gauche (Panneau du haut) ---
                        int hLeft =  static_cast<int>(normLeft * (paneHeight - 20));
                        SDL_FRect rectLeft{ (float)x, (float)(paneHeight - hLeft), (float)w, (float)hLeft };

                        uint8_t r = static_cast<uint8_t>( normLeft * 255.);
                        uint8_t g = static_cast<uint8_t>((1.-  normLeft) * 255.);
                        SDL_SetRenderDrawColor(renderer, r, 0, g, 255); // Cyan
                        SDL_RenderFillRect(renderer, &rectLeft);

                        // --- Canal Droit (Panneau du bas) ---
                        int hRight =  static_cast<int>(normRight * (paneHeight - 20));
                        r = static_cast<uint8_t>( normRight * 255.);
                        g = static_cast<uint8_t>((1. -  normRight) * 255.);
                        SDL_FRect rectRight{ (float)x, (float)(paneHeight ), (float)w, (float)hRight };
                        SDL_SetRenderDrawColor(renderer, r,0,g, 255); // Magenta
                        SDL_RenderFillRect(renderer, &rectRight);
                    }

            // Ligne de séparation médiane
                    //SDL_SetRenderDrawColor(renderer, 255, 255, 255, 100);
                    //SDL_RenderLine(renderer, 0, paneHeight, WINDOW_WIDTH, paneHeight);


    }




struct geometry_spectrum_settings {
    int num_bars = FFT_SIZE;
    float bar_width = 2.0f;
    float spacing = 0.0f;
    float max_height = 250.0f;
    float peak_thickness = 0.0f;
    float gravity = 1.5f;
    int hold_time = 15;

    // Dégradé pour le Canal Gauche (Haut)
    SDL_FColor color_center_l = { 0.0f, 0.0f, 1.0f, 1.0f }; // Vert néon au centre (Y = center)
    SDL_FColor color_top_l    = { 1.0f, 0.0f, 0.0f, 1.0f }; // Bleu électrique au sommet

    // Dégradé pour le Canal Droit (Bas - Miroir)
    SDL_FColor color_center_r = { 0.0f, 0.0f, 1.f, 0.1f }; // Idem Vert néon au centre
    // Le rouge/rose fluo pour le bas crée une asymétrie esthétique superbe
    SDL_FColor color_bottom_r = { 1.0f, 0.0f, 0.f, 0.1f }; 

    // Couleur unie pour la ligne de crête
    SDL_FColor color_peak     = { 0.0f, 0.0f, 0.0f, 0.1f }; // Blanc pur
};

struct peak_state {
    std::vector<float> positions_l; // Hauteurs actuelles des crêtes (Gauche)
    std::vector<float> positions_r; // Hauteurs actuelles des crêtes (Droite)
    std::vector<int> hold_counters_l; // Compteurs de maintien au sommet
    std::vector<int> hold_counters_r;

    void Init(int num_bars) {
        positions_l.assign(num_bars, 0.0f);
        positions_r.assign(num_bars, 0.0f);
        hold_counters_l.assign(num_bars, 0);
        hold_counters_r.assign(num_bars, 0);
    }
};


// Convertit une magnitude linéaire [0.0, 1.0] en un ratio de hauteur [0.0, 1.0] basé sur les dB
inline float magnitude_toDb_ratio(float magnitude) {
    const float min_db = -60.0f; // Plancher de bruit (le bas de votre barre = -60 dB)
    
    // 1. Sécurité anti-zéro : on applique un seuil minuscule pour éviter log10(0)
    if (magnitude < 1e-5f) {
        return 0.0f; // Sous les -100 dB, la barre reste à zéro
    }

    // 2. Calcul du niveau en dB
    float db = 20.0f * std::log10(magnitude);

    // 3. Écrêtage (Clamping) pour rester dans la plage cible [-60 dB, 0 dB]
    if (db < min_db) db = min_db;
    if (db > 0.0f)   db = 0.0f;

    // 4. Normalisation linéaire de la plage de dB vers un ratio [0.0f, 1.0f]
    // -60 dB deviendra 0.0f (hauteur nulle)
    //   0 dB deviendra 1.0f (hauteur maximale)
    float ratio = 1.0f - (db / min_db); 
    
    return ratio;
}


std::vector<SDL_Vertex> v_left, v_right, v_peaks;
std::vector<int> i_left, i_right, i_peaks;
std::vector<float> fft_left, fft_right;

inline void render_geometry_mirror_spectrum
(
    SDL_Renderer* renderer,
    const geometry_spectrum_settings* settings, 
    peak_state *peaks,
    int window_width, int window_height,
    sse2_fft *fft, 
    SDL_AudioStream* playback_stream , 
    SDL_AudioStream* capture_stream, 
    std::vector<float> *buffer
) 
{

    int buffer_size = SDL_GetAudioStreamAvailable( capture_stream );  // number of bytes the stream has accumulated so far.
            
    if ( playback.load() == true && buffer_size > 0)
    {
        std::vector<float> buff(buffer_size);
        size_t frame_size_in_bytes = sizeof(float) * 2;

        SDL_GetAudioStreamData(capture_stream,buff.data(),buffer_size);

        last_audio_cursor_stream = audio_cursor_stream;
        audio_cursor_stream.store(std::min( audio_cursor_stream + buffer_size, buffer->size()  - 1) );

        memcpy(buffer->data() + last_audio_cursor_stream, buff.data(), (audio_cursor_stream-last_audio_cursor_stream) *sizeof(float) );
        
        int samplesbuffered = audio_cursor_stream;
        if (samplesbuffered == buffer->size()  - 1) 
        { 
            audio_cursor_stream.store( audio_cursor_stream.load() % buffer->size()  - 1 );
        
            fft->process_audio_spectrum(buffer,&fft_left,&fft_right );

        // playback
            SDL_PutAudioStreamData(playback_stream,buff.data(),buffer_size );
        }
    }

        float center_y = window_height / 2.0f;
        float total_width = (settings->num_bars * settings->bar_width) + ((settings->num_bars - 1) * settings->spacing);
        float start_x = (window_width - total_width) / 2.0f;

        // Tableaux de géométrie globaux pour grouper le rendu (Batching)
            v_left.resize(0);
            v_right.resize(0);
            v_peaks.resize(0);

        // Lambda helper pour pousser un rectangle (4 sommets + 6 indices) avec couleurs par sommets
        auto push_rectangle_geometry = [](std::vector<SDL_Vertex>& vertices, std::vector<int>& indices,
                                        float x, float y, float w, float h, 
                                        SDL_FColor c_top_left, SDL_FColor c_top_right, 
                                        SDL_FColor c_bot_left, SDL_FColor c_bot_right) 
        {
            int base_idx = (int)vertices.size();

            // 4 Sommets du rectangle
            SDL_Vertex tl = { {x, y},     c_top_left,  {0,0} }; // Top Left
            SDL_Vertex tr = { {x+w, y},   c_top_right, {0,0} }; // Top Right
            SDL_Vertex bl = { {x, y+h},   c_bot_left,  {0,0} }; // Bottom Left
            SDL_Vertex br = { {x+w, y+h}, c_bot_right, {0,0} }; // Bottom Right

            vertices.push_back(tl); vertices.push_back(tr);
            vertices.push_back(bl); vertices.push_back(br);

            // 6 Indices formant 2 triangles (Sens horaire : TL->TR->BR et TL->BR->BL)
            indices.push_back(base_idx + 0); indices.push_back(base_idx + 1); indices.push_back(base_idx + 3);
            indices.push_back(base_idx + 0); indices.push_back(base_idx + 3); indices.push_back(base_idx + 2);
        };

        // Boucle de génération de la géométrie
        for (int i = 0; i < settings->num_bars; ++i) {
            float magnitude_l = (i < fft_left.size())  ? fft_left[i] *= 0.95  : 0.0f;
            float magnitude_r = (i < fft_right.size()) ? fft_right[i] *= 0.95 : 0.0f;
            
            float height_l = magnitude_toDb_ratio(magnitude_l) * settings->max_height;
            float height_r = magnitude_toDb_ratio(magnitude_r) * settings->max_height;
            float current_x = start_x + i * (settings->bar_width + settings->spacing);

            // --- TRAITEMENT PHYSIQUE DES CRÊTES (Identique) ---
          /*  if (height_l >= peaks->positions_l[i]) { peaks->positions_l[i] = height_l; peaks->hold_counters_l[i] = settings->hold_time; }
            else { if (peaks->hold_counters_l[i] > 0) peaks->hold_counters_l[i]--; else { peaks->positions_l[i] -= settings->gravity; if (peaks->positions_l[i] < 0.0f) peaks->positions_l[i] = 0.0f; } }

            if (height_r >= peaks->positions_r[i]) { peaks->positions_r[i] = height_r; peaks->hold_counters_r[i] = settings->hold_time; }
            else { if (peaks->hold_counters_r[i] > 0) peaks->hold_counters_r[i]--; else { peaks->positions_r[i] -= settings->gravity; if (peaks->positions_r[i] < 0.0f) peaks->positions_r[i] = 0.0f; } }
            */

            // --- GÉNÉRATION GÉOMÉTRIE : CANAL GAUCHE (HAUT) ---
            // Le haut a la couleur 'color_top_l' et le bas (sur la ligne centrale) a 'color_center_l'
            push_rectangle_geometry(v_left, i_left, 
                                current_x, center_y - height_l, settings->bar_width, height_l,
                                settings->color_top_l, settings->color_top_l, 
                                settings->color_center_l, settings->color_center_l);

            // Crête Gauche
            //push_rectangle_geometry(v_peaks, i_peaks,
            //                    current_x, center_y - peaks->positions_l[i] - settings->peak_thickness, settings->bar_width, settings->peak_thickness,
            //                    settings->color_peak, settings->color_peak, settings->color_peak, settings->color_peak);

            // --- GÉNÉRATION GÉOMÉTRIE : CANAL DROIT (BAS - MIROIR) ---
            // Le haut (sur la ligne centrale) a 'color_center_r' et le bas a 'color_bottom_r'
            push_rectangle_geometry(v_right, i_right,
                                current_x, center_y, settings->bar_width, height_r,
                                settings->color_center_r, settings->color_center_r,
                                settings->color_bottom_r, settings->color_bottom_r);

            // Crête Droite
          //  push_rectangle_geometry(v_peaks, i_peaks,
          //                      current_x, center_y + peaks->positions_r[i], settings->bar_width, settings->peak_thickness,
          //                      settings->color_peak, settings->color_peak, settings->color_peak, settings->color_peak);

        SDL_SetRenderDrawColor(renderer, 10, 10, 10, 255);
        SDL_RenderClear(renderer);
        // --- ENVOI DE LA GÉOMÉTRIE AU GPU (BATCHED RENDERING) ---
        // Mode de fusion standard pour gérer la translucidité des couleurs si vous baissez l'alpha
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

        // 1. Dessiner toutes les barres de gauche d'un coup

            SDL_RenderGeometry(renderer, NULL, v_left.data(), (int)v_left.size(), i_left.data(), (int)i_left.size());
        
        // 2. Dessiner toutes les barres de droite d'un coup

            SDL_RenderGeometry(renderer, NULL, v_right.data(), (int)v_right.size(), i_right.data(), (int)i_right.size());
        
        // 3. Dessiner toutes les barres de crêtes d'un coup

            SDL_RenderGeometry(renderer, NULL, v_peaks.data(), (int)v_peaks.size(), i_peaks.data(), (int)i_peaks.size());
        }

                            // Ligne de séparation médiane
            SDL_SetRenderDrawColor(renderer, 255, 255, 255, 100);
            SDL_RenderLine(renderer, 0, paneHeight, WINDOW_WIDTH, paneHeight);
        
    

}

    struct audio_file_data_t
    {
        std::string path;
        float duration;
        std::string title;
        size_t channels;
        size_t sample_rate;
        std::vector<float> samples;
    };

    std::hash<std::string> string_hasher;

    std::unordered_map<size_t, audio_file_data_t > songs;

    static void load_audio_file(const char* file_path, std::vector<float> *samples)
    {

        std::string path = file_path;

        std::thread ([](std::string file,std::vector<float> *samples){

            auto start = std::chrono::high_resolution_clock::now();
            
            uint c = 2;
            uint s = 44100;

            error_file_loading_msg = "";
            
            size_t hash = string_hasher(file);

            if ( songs.find(hash) != songs.end() )
            {
                file_loading.store(false);
                error_file_loading.store(false);
                error_file_loading_msg = "file already loaded";
                return;
            }

            auto path = std::filesystem::path(file);
            std::string extension = path.extension().string();
            std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);
            std::string file_name = path.stem().string();

            int status = extension == ".wav" ? 1 : extension == ".flac" ? 2 : extension == ".mp3" ? 3 : 0;

            bool success = false;
            switch(status)
            {
            case 0:
            {
                return;
            }
            break;
            case 1:
                success = load_wav_file(file, samples,&c, &s);
                break;
            case 2:
                success = load_flac_file(file, samples,&c, &s);
                break;
            case 3:
                success = load_mp3_file(file, samples,&c, &s);
            }

            if ( success )
            {
                audio_file_data_t data = 
                {
                    .path = file,
                    .duration = ( static_cast<float>(samples->size())/static_cast<float>(c)/static_cast<float>(s) ),
                    .title = path.filename().string(),
                    .channels = c,
                    .sample_rate = s
                };
                data.samples.resize(samples->size());
                memcpy(data.samples.data(), samples->data(), samples->size() * sizeof(float));

                songs.emplace(hash, std::move(data) );

                channels.store(c);
                sample_rate.store(s);
                file_loaded.store(true );
                update_playback_stream.store(true);
            }
            file_loading.store(false);
            error_file_loading.store(!success);

        }, path, samples).detach();
    };

bool ImGuiStopButton(const char* str_id, ImVec2 size) {
    ImGuiContext& g = *GImGui;
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems) return false;


    const ImGuiID id = window->GetID(str_id);
    const ImRect bb(window->DC.CursorPos, {window->DC.CursorPos[0] + size[0],window->DC.CursorPos[1] + size[1]} );
    
    ImGui::ItemSize(size, g.Style.FramePadding.y);
    if (!ImGui::ItemAdd(bb, id)) return false;

    bool hovered, held;
    bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held);


    ImU32 bg_col = ImGui::GetColorU32((held && hovered) ? ImGuiCol_ButtonActive : hovered ? ImGuiCol_ButtonHovered : ImGuiCol_Button);
    ImU32 icon_col = ImGui::GetColorU32(ImGuiCol_Text);

    window->DrawList->AddRectFilled(bb.Min, bb.Max, bg_col, g.Style.FrameRounding);


    float padding = size.y * 0.25f; // Marges internes pour que l'icône ne colle pas aux bords
    ImVec2 stop_min(bb.Min.x + padding, bb.Min.y + padding);
    ImVec2 stop_max(bb.Max.x - padding, bb.Max.y - padding);


    window->DrawList->AddRectFilled(stop_min, stop_max, icon_col);

    return pressed;
}

bool ImGuiPlayButton(const char* label_id, ImVec2 size) {

    bool pressed = ImGui::Button(label_id, size);

    ImVec2 pos_min = ImGui::GetItemRectMin();
    ImVec2 pos_max = ImGui::GetItemRectMax();

    float padding_x = size.x * 0.25f;
    float padding_y = size.y * 0.25f;

    ImVec2 p1(pos_min.x + padding_x, pos_min.y + padding_y);           // Sommet haut-gauche
    ImVec2 p2(pos_min.x + padding_x, pos_max.y - padding_y);           // Sommet bas-gauche
    ImVec2 p3(pos_max.x - padding_x, pos_min.y + (size.y * 0.5f));     // Sommet pointe droite

    ImU32 icon_col = ImGui::GetColorU32(ImGuiCol_Text);

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddTriangleFilled(p1, p2, p3, icon_col);

    return pressed;
}

int main(int argc, char* argv[]) 
{


    geometry_spectrum_settings settings; 
    peak_state peaks;
    peaks.Init(settings.num_bars);
    peaks.hold_counters_l.resize(settings.num_bars);
    peaks.hold_counters_r.resize(settings.num_bars);
    peaks.positions_l.resize(settings.num_bars);
    peaks.positions_r.resize(settings.num_bars);

    sse2_fft fft_sse2(FFT_SIZE);

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
        printf("Erreur d'initialisation SDL: %s\n", SDL_GetError());
        return -1;
    }

    SDL_Window* window = SDL_CreateWindow("Spectrogramme 2D - SDL3", WINDOW_WIDTH, WINDOW_HEIGHT, SDL_WINDOW_RESIZABLE);
    
    if (!window) {
        printf("Erreur de création de la fenêtre: %s\n", SDL_GetError());
        return -1;
    }
    
    SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);
    if (!renderer) 
    {
        return -1;
    }

    SDL_Texture*  texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_STREAMING, WINDOW_WIDTH, WINDOW_HEIGHT);

    SDL_Window* window_ui = SDL_CreateWindow("Controles", 500, 400, SDL_WINDOW_TRANSPARENT );
    SDL_Renderer* renderer_ui = SDL_CreateRenderer(window_ui, NULL);
    if (!renderer_ui) return -1;

    // Forcer le positionnement initial de la fenêtre UI sur le bureau (ex: x=100, y=100)
    SDL_SetWindowPosition(window_ui, 100, 100);

    //  Initialiser le contexte Dear ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard; // Touches clavier
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;  // Manette
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable; 


    static ImGui::FileBrowser fileDialog(ImGuiFileBrowserFlags_CloseOnEsc);

// Variable pour stocker le chemin du fichier audio ou de configuration choisi
    static std::string selected_file_path = "Aucun fichier sélectionné";


    // 3. Configurer les styles d'ImGui (ex: thème sombre)
    ImGui::StyleColorsDark();

    ImGuiStyle& style = ImGui::GetStyle();
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        style.WindowRounding = 0.0f;
        style.Colors[ImGuiCol_WindowBg].w = 1.0f;
    }

    ImGui_ImplSDL3_InitForSDLRenderer(window_ui, renderer_ui);
    ImGui_ImplSDLRenderer3_Init(renderer_ui);

    spectrogram_renderer spectrogram(renderer,texture);

    SDL_Surface *surface =  SDL_GetWindowSurface(window);

    int pitch;
    void *pixels;
    SDL_LockTexture(texture, nullptr, &pixels, &pitch);
    SDL_UnlockTexture(texture);
    
    
    bool running = true;
    SDL_Event event;
    std::vector<float> localMagnitudes(FFT_SIZE / 2, 0.0f);

    std::vector<float> samples;

    int play_position;

      /*
    std::thread playback = std::thread([&,channels,sample_rate](spectrogram_renderer *spectrogram)->void
    {
        float buffer[2048];  
            // Configuration Audio SDL3 (Mono, 48kHz)
        SDL_AudioSpec spec{ SDL_AUDIO_F32, channels, sample_rate };
        SDL_AudioStream* stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL,NULL);
       
        const int minimum_audio = ( sample_rate * sizeof(float) ) / 2;  //  Half of samples per seconds 
        // SDL_OpenAudioDeviceStream starts the device paused. You have to tell it to start! 
        SDL_ResumeAudioStreamDevice(stream);

        while ( stop_playback_requested.load() != true && samples_cursor < samples.size() )
        {
            if (SDL_GetAudioStreamQueued(stream) < minimum_audio)
            {
                // this will feed 1024 samples each frame until we get to our maximum. 
                // generate samples from grooves 
                next_cursor = std::min(samples.size() - 1, samples_cursor + SDL_arraysize(buffer)) ;
                memcpy(buffer, (const void *)(samples.data()+samples_cursor), (next_cursor - samples_cursor) * sizeof(float) );

                // feed the new data to the stream. It will queue at the end, and trickle out as the hardware needs more data. 
                SDL_PutAudioStreamData(stream, buffer, (next_cursor - samples_cursor) * sizeof(float) );
                samples_cursor = next_cursor;
                if ( samples_cursor > samples.size() - 1 )
                {
                    break;
                }

            int samplesNeeded = SDL_arraysize(buffer);
                if (samplesNeeded >= FFT_SIZE) 
                { 
                    
                    
#if 0
                    {
                    // Récupérer les données du flux
                    // Préparer les données pour la FFT complexe
                    std::vector<std::complex<float>> fftData(FFT_SIZE);
                    for (int i = 0; i < FFT_SIZE; ++i) 
                    {
                        // Optionnel mais recommandé : appliquer une fenêtre de Hamming ici pour éviter le "spectral leakage"
                        fftData[i] = std::complex<float>(buffer[i], 0.0f);
                    }
                    spectrogram->fft(&fftData);
                    // Extraction des amplitudes de la moitié positive du spectre

                    for (int i = 0; i < FFT_SIZE / 2; ++i) {
                        spectrogram->g_latestMagnitudes[i] = std::abs(fftData[i]);
                    }

                    spectrogram->addFFTFrame(&spectrogram->g_latestMagnitudes);
                    }
#endif                   

                    spectrogram->update_frame(buffer,SDL_arraysize(buffer) );

                }
                
            }
        }
    
            SDL_DestroyAudioStream(stream);
            // Mise à jour de l'image défilante
    

        },&spectrogram
    );

    */


            // Configuration Audio SDL3 (Mono, 48kHz)
        
        SDL_AudioStream* playback_stream = nullptr;

        SDL_AudioSpec spec{ SDL_AUDIO_F32, channels.load(), sample_rate.load() };
        SDL_AudioStream* capture_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_RECORDING, &spec, NULL,NULL);
       
        const int minimum_audio = ( sample_rate * sizeof(float) ) / 2;  //  Half of samples per seconds 

        // 1. Liste des options disponibles pour l'analyseur
        const char* fft_visualizer_style[] = { 
            "spectrum analyzer", 
            "spectrogram", 
            "capture to playback"
        };

    // Index de l'élément actuellement sélectionné (à déclarer en variable persistante/globale)
    static int current_window_idx = 2; 

    // Libellé affiché à l'écran basé sur l'élément sélectionné
    const char* combo_preview_value = fft_visualizer_style[current_window_idx];


    bool selected_visualizer = false;

        alignas(64) std::atomic<bool> close_dialog = false;

        int w = WINDOW_WIDTH,h = WINDOW_HEIGHT;

    std::chrono::time_point<std::chrono::high_resolution_clock> start_playing_time;

    double total_playing_time;

    float gain_value = -20.;

    std::vector<float> signal(256,0.); 
    std::deque<float> signal_queue(256,0.); 



    size_t id_selectionne = -1;
    size_t id_a_supprimer = -1;
    // Notre map de données


    
    
    while (running ) 
    {
        while (SDL_PollEvent(&event)) 
        {

            ImGui_ImplSDL3_ProcessEvent(&event);

            if ( event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED )
            {
                Uint32 closed_id = event.window.windowID;
                if (closed_id == SDL_GetWindowID(window)) {
                    running = false; // Fermer tout si la fenêtre principale se ferme
                }
                //if (closed_id == SDL_GetWindowID(window_ui)) {
                //    SDL_HideWindow(window_ui); // Masquer simplement l'UI si on clique sur sa croix
               // }
            }
            else if ( event.type == SDL_EVENT_WINDOW_RESIZED)
            {
                SDL_GetWindowSize(window,&w,&h);

            }
        }



        switch(current_window_idx)
        {
            case 0:
            process_spectrum_play_back(paneHeight,minimum_audio, playback_stream,renderer, &samples,&buffer);
            break;
            case 1:
            break;
            case 2:
            {
            //render_geometry_mirror_spectrum(renderer,&settings,&peaks,w,h, &fft_sse2, playback_stream, capture_stream, &buffer );
              process_spectrum_capture_to_playback(renderer, playback_stream,capture_stream,&buffer);
            }
            break;
        }


        SDL_RenderPresent(renderer);


        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        // Construit l'interface pour qu'elle prenne toute la place de la fenêtre UI
        int ui_w, ui_h;
        SDL_GetWindowSize(window_ui, &ui_w, &ui_h);
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2((float)ui_w, (float)ui_h));

        // Fenêtre ImGui fixe (sans titre ni bordure interne car c'est la fenêtre OS qui fait office de cadre)
        ImGuiWindowFlags ui_flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse;
        
        ImGui::Begin("menu", nullptr, ui_flags);
        ImGui::Separator();


        // Section Configuration
        ImGui::TextColored(ImVec4(0.0f, 1.0f, 1.0f, 1.0f), "sprectum visualizer");
        ImGui::Separator();

        ImGui::Combo("spectrum visualizer", &current_window_idx, fft_visualizer_style, IM_ARRAYSIZE(fft_visualizer_style));
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::Separator();
        if( ImGui::SliderFloat("Gain (dB)", &gain_value, -90, 0.) )
        {
            gain_db.store(gain_value);
            gain.store( std::pow( 10.f,gain_db/20.f ) );

        }
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::Separator();
        
        // if capture to playback not selected
        if (current_window_idx != 2 && playback.load() == false )
        {
            ImGui::Text("choose audio file :");
            ImGui::TextUnformatted(selected_file_path.c_str());
            // 2. Bouton pour ouvrir l'explorateur
            if (ImGui::Button("open audio file...")) {
                // Configurer le titre et les extensions autorisées
                fileDialog.SetTitle("Choisir un fichier audio (.wav, .flac, .mp3)");
                fileDialog.SetTypeFilters({ ".wav", ".mp3", ".flac" });
                
                // Ouvrir la boîte de dialogue
                fileDialog.Open();
            }

            if (fileDialog.HasSelected()) 
            {

                selected_file_path = fileDialog.GetSelected().string();
                
                if ( !selected_file_path.empty() )
                {
                    file_loading.store(true);
                    current_playing_time = 0.;
                    std::thread( [](std::string file, std::vector<float> *samples, SDL_AudioStream* playback_stream )
                    {
                        load_audio_file(file.c_str(),samples);
                    },
                    selected_file_path,&samples,playback_stream).detach();
                }
                close_dialog.store(true);
                fileDialog.ClearSelected(); // Réinitialiser le dialogue
            }

            if (file_loading)
            {
                std::string m =std::string("loading file ...")+selected_file_path;
                ImGui::Text(m.c_str());
            }

            if (error_file_loading.load() == true )
            {
                std::string m =std::string("error: unable to load file ...")+selected_file_path;
                if ( !error_file_loading_msg.empty())
                {
                    m = error_file_loading_msg;
                }
                ImGui::Text(m.c_str());
            }


            if (close_dialog.load() )
            {
                fileDialog.Close();
                close_dialog.store(false);
            }

            if (fileDialog.IsOpened()) 
            {
                ImGui::SameLine();
                if (ImGui::Button("close")) 
                {
                    fileDialog.Close();
                }
                // Récupérer la taille actuelle de votre fenêtre SDL d'interface (window_ui)
                int ui_w, ui_h;
                SDL_GetWindowSize(window_ui, &ui_w, &ui_h);
                
                // On force la fenêtre de l'explorateur à être légèrement plus petite 
                // que la fenêtre système pour que la croix 'X' reste accessible.
                ImGui::SetNextWindowSize(ImVec2((float)ui_w - 40.0f, (float)ui_h - 60.0f));
                ImGui::SetNextWindowPos(ImVec2(20.0f, 40.0f));
            }

            fileDialog.Display();
        }
        if ( file_loaded.load() == true || current_window_idx == 2)
        {
            ImGui::Separator();
            if ( file_loaded && current_window_idx != 2 )
            {
                if ( playback.load() == false)
                {
                    if ( update_playback_stream.load() == true)
                    {
                        SDL_AudioSpec spec{ SDL_AUDIO_F32, channels.load(), sample_rate.load() };
                        if ( playback_stream != nullptr)
                        {
                            SDL_PauseAudioStreamDevice(playback_stream);
                            SDL_ClearAudioStream(playback_stream);
                            SDL_DestroyAudioStream(playback_stream);
                        }
                        playback_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL,NULL);
                        if ( playback_stream == nullptr)
                        {
                            error_file_loading_msg = "audio output could not be initialized";
                        }
                        else
                        {
                            SDL_ResumeAudioStreamDevice(playback_stream);
                            update_playback_stream.store(false);
                        }

                    }
                   ImGui::Text("file ready to play");
                }
                else
                { 
                    std::string c = duration_to_hhmmss(current_playing_time);
                    std::string d = std::string("playing file: ")+c+std::string("/")+duration_to_hhmmss(total_playing_time);
                    ImGui::Text(d.c_str());
                    bool change_play_position = ImGui::SliderInt("##playing_position", &play_position, 0, samples.size() - 1,c.c_str(),0);
                    if (!change_play_position)
                    {
                        play_position = samples_cursor.load();
                    }
                    else 
                    {
                        audio_cursor_stream.store( play_position );
                        samples_cursor.store( play_position );
                        current_playing_time = play_position/sample_rate/channels;
                    }
                    

                    if (samples_cursor.load() < samples.size() )
                    {
                        signal_queue.push_back( *(samples.data() + samples_cursor.load() ) );
                        signal_queue.pop_front();
                        signal = std::vector<float>(signal_queue.begin(),signal_queue.end() );
                        ImGui::PushStyleColor(ImGuiCol_PlotLines, ImVec4(1.0f, 0.4f, 0.4f, 1.0f)); // Courbe Rouge
                        ImGui::PlotLines("Samples", signal.data(), 256,0,"samples",-1.1f, 1.1f, ImVec2(0, 50));
                        ImGui::PopStyleColor();
                    }
                }
            }
            ImGui::Spacing(); 
            ImGui::Dummy(ImVec2(0.0f, 25.0f)); 
            ImGui::Separator();
            // Bouton Play

            if (playback.load() == false && ImGuiPlayButton("##PlayBtn", ImVec2(32.0f, 32.0f)) ) 
            {
                play_position = 0;
                last_audio_cursor_stream = 0;
                audio_cursor_stream = 0;
                samples_cursor = 0;
                next_cursor = 0;
                SDL_ResumeAudioStreamDevice(playback_stream);
                if ( current_window_idx == 2)
                {
                    SDL_ResumeAudioStreamDevice(capture_stream);
                }
                
                playback.store(true);
                total_playing_time = samples.size()/channels / sample_rate;
                start_playing_time = now();
                
            }

            if (playback.load() == false)
            {
                ImGui::SameLine();
                if (ImGuiStopButton("##AudioStop", ImVec2(32.0f, 32.0f))) 
                {
                    last_audio_cursor_stream = 0;
                    audio_cursor_stream = 0;
                    samples_cursor = 0;
                    next_cursor = 0;
                    SDL_PauseAudioStreamDevice(playback_stream);
                    SDL_ClearAudioStream(playback_stream); 
                    if ( current_window_idx == 2)
                    {
                        SDL_PauseAudioStreamDevice(capture_stream);
                        SDL_ClearAudioStream(capture_stream); 
                    }
                    playback.store(false);
                }
            }
            else
            if ( ImGuiStopButton("##AudioStop", ImVec2(32.0f, 32.0f))) 
            {
                last_audio_cursor_stream = 0;
                audio_cursor_stream = 0;
                samples_cursor = 0;
                next_cursor = 0;
                SDL_PauseAudioStreamDevice(playback_stream);
                SDL_ClearAudioStream(playback_stream); 
                if ( current_window_idx == 2)
                {
                    SDL_PauseAudioStreamDevice(capture_stream);
                    SDL_ClearAudioStream(capture_stream); 
                }
                playback.store(false);
            }

            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Lancer la lecture");

            if (ImGui::IsItemHovered()) 
            {
                ImGui::SetTooltip("stop playback");
            }
            ImGui::Separator();
            for (auto const& [id, element] : songs)
            {
                // 1. Configuration des drapeaux (Flags)
                ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick;
                
                // Mettre en surbrillance si cet ID est sélectionné
                if (id_selectionne == id) {
                    flags |= ImGuiTreeNodeFlags_Selected;
                }

                // Optionnel : Si vous n'avez pas d'enfants à afficher pour cet élément, 
                // vous pouvez le traiter comme une feuille directement :
                // flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;

                // 2. Rendu du nœud en utilisant l'ID unique de la map
                // On caste l'ID (uint64_t) en (void*)(uintptr_t) pour le système d'ID d'ImGui
                bool node_ouvert = ImGui::TreeNodeEx((void*)(uintptr_t)id, flags, "%s", element.title.c_str());

                // 3. Gestion de la sélection au clic
                // !ImGui::IsItemToggledOpen() évite de sélectionner l'élément si on clique juste sur la flèche
                if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
                    id_selectionne = id;
                }

                // --- DEBUT DU MENU CONTEXTUEL (CLIC DROIT) ---
                // Cette fonction s'associe automatiquement au dernier TreeNodeEx affiché ci-dessus
                if (ImGui::BeginPopupContextItem()) 
                {
                    // Option de sélection rapide au clic droit
                    if (ImGui::MenuItem("Sélectionner")) {
                        id_selectionne = id;
                    }
                    
                    ImGui::Separator();

                    // Option de suppression stylisée en rouge
                    if (ImGui::MenuItem("Supprimer", nullptr, false, true)) {
                        id_a_supprimer = id; // On mémorise l'ID, on NE supprime PAS tout de suite
                    }

                    ImGui::EndPopup();
                }

                // 4. Affichage du contenu intérieur si le nœud est développé
                if (node_ouvert)
                {
                    // On affiche et modifie directement les données de la structure

                    ImGui::Text("path :      %s", songs[id].path.c_str());
                    ImGui::Text("duration    %s", duration_to_hhmmss((double) songs[id].duration).c_str() );
                    ImGui::Text("channels    %zu", songs[id].channels);
                    ImGui::Text("sample rate %zu", songs[id].sample_rate);
                    
                    // Obligatoire si node_ouvert est vrai et que NoTreePushOnOpen n'est pas utilisé
                    ImGui::TreePop(); 
                }
            }
            // 2. SUPPRESSION SÉCURISÉE (En dehors de la boucle de rendu)
            if (id_a_supprimer != -1) 
            {
                songs.erase(id_a_supprimer);
                
                // Si l'élément supprimé était celui sélectionné, on réinitialise la sélection
                if (id_selectionne == id_a_supprimer) {
                    id_selectionne = 0; 
                }
                
                id_a_supprimer = -1; // Réinitialisation
            }
        }

        ImGui::End();

        // 3. Rendu de la scène
        ImGui::Render(); // Calcule les géométries d'ImGui

        SDL_SetRenderDrawColor(renderer_ui, 45, 45, 45, 255);  // Fond gris moyen
        SDL_RenderClear(renderer_ui);
        // On force le dessin d'ImGui sur le renderer de la deuxième fenêtre
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer_ui);
        SDL_RenderPresent(renderer_ui);

    }
                


   // playback.join();

    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();

    SDL_DestroyRenderer(renderer_ui);
    SDL_DestroyWindow(window_ui);
    SDL_DestroyAudioStream(playback_stream);
    SDL_DestroyMutex(spectrogram.g_audioMutex);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyTexture(texture);
    SDL_DestroyWindow(window);

    SDL_Quit();
    return 0;
}