#include <thread>

#define DR_WAV_IMPLEMENTATION
#include "./dr_libs/dr_wav.h"
#include "./dr_libs/dr_flac.h"
#include "./dr_libs/dr_mp3.h"

#include "miniaudio.h"
#include "spectrogram_renderer.h"


inline void load_wav_file(std::string &path,std::vector<float> *interleaved_samples, uint *channels, uint *sample_rate)
{

    drwav_uint64 totalPCMFrameCount;

    float* pSampleData = drwav_open_file_and_read_pcm_frames_f32(path.c_str(), channels, sample_rate, &totalPCMFrameCount, NULL);
    
    if (pSampleData == NULL)
    {
        // Error opening and reading WAV file.
        return;
    }

    // build node data
    //  copy samples to node data structure
    interleaved_samples->resize(totalPCMFrameCount* *channels);
    memcpy(interleaved_samples->data(),pSampleData,totalPCMFrameCount* *channels * sizeof(float));
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
        static inline bool ma_read_from_wav_file(const char *file_name, std::vector<float> *interleaved_samples, size_t *channels, double *sample_rate)
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

int main(int argc, char* argv[]) {
    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO);


    SDL_Window* window = SDL_CreateWindow("Spectrogramme 2D - SDL3", WINDOW_WIDTH, WINDOW_HEIGHT, 0);
    SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);
    SDL_Texture*  texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_STREAMING, WINDOW_WIDTH, WINDOW_HEIGHT);
    spectrogram_renderer spectrogram(renderer,texture);

    

    bool running = true;
    SDL_Event event;
    std::vector<float> localMagnitudes(FFT_SIZE / 2, 0.0f);

    std::vector<float> samples;



    std::string f = "path/to/file"; // R3-099 - A.wav";

    //    size_t channels = 0; 
   // double sample_rate = 44100;
   // ma_read_from_wav_file(f.c_str(),&samples,&channels,&sample_rate);

    uint channels = 0; 
    uint sample_rate = 44100;
    load_wav_file(f, &samples,&channels,&sample_rate);

    size_t samples_cursor = 0, next_cursor = 0;

      alignas(64) std::atomic<bool> stop_playback = false;

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

        while ( stop_playback.load() != true && samples_cursor < samples.size() )
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

        float buffer[8192];  
            // Configuration Audio SDL3 (Mono, 48kHz)
        SDL_AudioSpec spec{ SDL_AUDIO_F32, channels, sample_rate };
        SDL_AudioStream* stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL,NULL);
       
        const int minimum_audio = ( sample_rate * sizeof(float) ) / 2;  //  Half of samples per seconds 
        // SDL_OpenAudioDeviceStream starts the device paused. You have to tell it to start! 
        SDL_ResumeAudioStreamDevice(stream);


    // Tampons pour la FFT courante
    std::vector<float> currentLeft = std::vector<float>(FFT_SIZE, 0.0f);
    std::vector<float> currentRight = std::vector<float>(FFT_SIZE, 0.0f);
    std::vector<std::complex<float>> fftLeft = std::vector<std::complex<float>>(FFT_SIZE); std::vector<std::complex<float>> fftRight = std::vector<std::complex<float>>(FFT_SIZE);

    size_t paneHeight = WINDOW_HEIGHT / 2;

    while (running && stop_playback.load() != true && samples_cursor < samples.size()) 
    {
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT)
            {
                 running = false;
                stop_playback.store(true);
            }
        }

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

                    // Rendu graphique
                    SDL_SetRenderDrawColor(renderer, 50, 50, 50, 255);
                    SDL_RenderClear(renderer);

                    for (size_t  i = 0; i < FFT_SIZE; ++i) 
                    {
                        currentLeft[i] = buffer[i * 2];
                        currentRight[i] = buffer[i * 2 + 1];
                    }

                    for (int i = 0; i < FFT_SIZE; ++i) {
                        float windowMultiplier = 0.5f * (1.0f - std::cos(2.0f * M_PI * i / (FFT_SIZE - 1)));
                        fftLeft[i] = std::complex<float>(currentLeft[i] * windowMultiplier, 0.0f);
                        fftRight[i] = std::complex<float>(currentRight[i] * windowMultiplier, 0.0f);
                    }
                    spectrogram.fft(&fftLeft);
                    spectrogram.fft(&fftRight);

                    int usableBins = FFT_SIZE /4; // Fréquences utiles uniques
                    float barWidth = static_cast<float>(WINDOW_WIDTH) / usableBins;
                    for (int i = 0; i < usableBins; ++i) {
                        // Calcul de l'amplitude (mise à l'échelle logarithmique visuelle)
                        float magLeft = std::abs(fftLeft[i]) / std::sqrt(FFT_SIZE);
                        float magRight = std::abs(fftRight[i]) / std::sqrt(FFT_SIZE);

                        float normLeft = std::clamp(std::log1p(magLeft * 20.0f) / 5.0f, 0.0f, 1.0f);
                        float normRight = std::clamp(std::log1p(magRight * 20.0f) / 5.0f, 0.0f, 1.0f);

                        int x = static_cast<int>(i * barWidth);
                        int w = std::max(1, static_cast<int>(barWidth));

                        // --- Canal Gauche (Panneau du haut) ---
                        int hLeft =  static_cast<int>(normLeft * (paneHeight - 20));
                        SDL_FRect rectLeft{ (float)x, (float)(paneHeight - hLeft), (float)w, (float)hLeft };
                        SDL_SetRenderDrawColor(renderer, 0, 180, 255, 255); // Cyan
                        SDL_RenderFillRect(renderer, &rectLeft);

                        // --- Canal Droit (Panneau du bas) ---
                        int hRight =  static_cast<int>(normRight * (paneHeight - 20));
                        SDL_FRect rectRight{ (float)x, (float)(paneHeight ), (float)w, (float)hRight };
                        SDL_SetRenderDrawColor(renderer, 255, 0, 128, 255); // Magenta
                        SDL_RenderFillRect(renderer, &rectRight);
                    }

                    // Ligne de séparation médiane
                    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 100);
                    SDL_RenderLine(renderer, 0, paneHeight, WINDOW_WIDTH, paneHeight);

                 //   SDL_RenderTexture(renderer, texture, nullptr, nullptr);
                            
                    SDL_RenderPresent(renderer);
                    //SDL_Delay(16); // ~60 FPS pour le défilement
                }
                
            }

        
    }

   // playback.join();

    SDL_DestroyAudioStream(stream);
    SDL_DestroyMutex(spectrogram.g_audioMutex);
    SDL_DestroyRenderer(renderer);
            SDL_DestroyTexture(texture);
    SDL_DestroyWindow(window);

    SDL_Quit();
    return 0;
}