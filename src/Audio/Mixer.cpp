/*
 * Copyright 2012-2018 Falltergeist Developers.
 *
 * This file is part of Falltergeist.
 *
 * Falltergeist is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Falltergeist is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Falltergeist.  If not, see <http://www.gnu.org/licenses/>.
 */

// Related headers
#include "../Audio/Mixer.h"

// C++ standard includes
#include <string>

// Falltergeist includes
#include "../Base/Buffer.h"
#include "../Exception.h"
#include "../Format/Acm/File.h"
#include "../Game/Game.h"
#include "../Logger.h"
#include "../ResourceManager.h"
#include "../Settings.h"
#include "../UI/MvePlayer.h"

// Third party includes
#include <SDL.h>

namespace Falltergeist
{
    namespace Audio
    {
        Mixer::Mixer()
        {
            _init();
        }

        Mixer::~Mixer()
        {
            for (auto& x: _sfx)
            {
                Mix_FreeChunk(x.second);
            }
            Mix_HookMusic(NULL,NULL);
            Mix_CloseAudio();
        }

        void Mixer::_init()
        {
            std::string message = "[AUDIO] - SDL_Init - ";
            if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0)
            {
                Logger::critical() << message + "[FAIL]" << std::endl;
                throw Exception(SDL_GetError());
            }
            Logger::info() << message + "[OK]" << std::endl;

            message = "[AUDIO] - Mix_OpenAudio - ";
            if (Mix_OpenAudio(22050, AUDIO_S16LSB, 2, Game::getInstance()->settings()->audioBufferSize()) < 0)
            {
                Logger::critical() << message + "[FAIL]" << std::endl;
                throw Exception(Mix_GetError());
            }
            Logger::info() << message + "[OK]" << std::endl;
            int frequency, channels;
            Mix_QuerySpec(&frequency, &_format, &channels);
        }

        void Mixer::stopMusic()
        {
            Mix_HookMusic(NULL, NULL);
        }

        std::function<void(void*, uint8_t*, uint32_t)> musicCallback;

        void myMusicPlayer(void *udata, uint8_t *stream, int len)
        {
            musicCallback(udata, stream, len);
        }

        void Mixer::_musicCallback(void *udata, uint8_t *stream, uint32_t len)
        {
            if (_paused) return;

            auto pacm = (Format::Acm::File*)(udata);
            if (pacm->samplesLeft() <= 0)
            {
                if (_loop)
                {
                    pacm->rewind();
                }
                else
                {
                    Mix_HookMusic(NULL,NULL);
                    return;
                }
            }

            // music is stereo. just fetch
            Base::Buffer<uint16_t> tmp(len / 2);
            pacm->readSamples(tmp.data(), len / 2);
            SDL_memset(stream, 0, len);
            SDL_MixAudioFormat(stream, (uint8_t*)tmp.data(), _format, len, static_cast<int>(SDL_MIX_MAXVOLUME * _musicVolume));
        }

        void Mixer::playACMMusic(const std::string& filename, bool loop)
        {
            Mix_HookMusic(NULL, NULL);
            auto acm = ResourceManager::getInstance()->acmFileType(Game::getInstance()->settings()->musicPath()+filename);
            if (!acm) return;
            _lastMusic = filename;
            _loop = loop;
            musicCallback = std::bind(&Mixer::_musicCallback, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3);
            acm->rewind();
            Mix_HookMusic(myMusicPlayer, (void *)acm);
        }

        void Mixer::_speechCallback(void *udata, uint8_t *stream, uint32_t len)
        {
            if (_paused) return;

            auto pacm = (Format::Acm::File*)(udata);
            if (pacm->samplesLeft() <= 0)
            {
                Mix_HookMusic(NULL,NULL);
                return;
            }

            Base::Buffer<uint16_t> tmp(len / 2);
            uint16_t* sstr = (uint16_t*)stream;
            pacm->readSamples(tmp.data(), len / 4);
            for (size_t i = 0; i < len / 4; i++)
            {
                sstr[i*2] = tmp[i];
                sstr[i*2 + 1] = tmp[i];
            }
        }

        void Mixer::playACMSpeech(const std::string& filename)
        {
            Mix_HookMusic(NULL, NULL);
            auto acm = ResourceManager::getInstance()->acmFileType("sound/speech/"+filename);
            if (!acm) return;
            musicCallback = std::bind(&Mixer::_speechCallback, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3);
            acm->rewind();
            Mix_HookMusic(myMusicPlayer, (void *)acm);
        }

        void Mixer::_movieCallback(void *udata, uint8_t *stream, uint32_t len)
        {
            auto pmve = (UI::MvePlayer*)(udata);
            if (pmve->samplesLeft() <= 0)
            {
                Logger::debug("AUDIO") << "buffer underrun?" << std::endl;
                Mix_HookMusic(NULL, NULL);
                return;
            }

            pmve->getAudio(stream, len);
        }

        void Mixer::playMovieMusic(UI::MvePlayer* mve)
        {
            musicCallback = std::bind(&Mixer::_movieCallback,this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3);
            Mix_HookMusic(myMusicPlayer, reinterpret_cast<void *>(mve));
        }

        void Mixer::playACMSound(const std::string& filename)
        {
            auto acm = ResourceManager::getInstance()->acmFileType(filename);
            if (!acm) return;
            Logger::debug("Mixer") << "playing: " << acm->filename() << std::endl;
            Mix_Chunk *chunk = NULL;

            auto it = _sfx.find(acm->filename());
            if (it != _sfx.end())
            {
                chunk=it->second;
            }

            if (!chunk)
            {
                auto samples = acm->samples();

                Base::Buffer<uint16_t> tmpSamples(samples);
                auto cnt = acm->readSamples(tmpSamples.data(), samples) * 2;

                SDL_AudioCVT cvt;
                SDL_BuildAudioCVT(&cvt, AUDIO_S16LSB, 1, 22050, AUDIO_S16LSB, 2, 22050); //convert from mono to stereo

                cvt.buf = (Uint8*)malloc(cnt * cvt.len_mult);
                memcpy(cvt.buf, tmpSamples.data(), cnt);
                cvt.len = static_cast<int>(cnt);
                SDL_ConvertAudio(&cvt);

                // make SDL_mixer chunk
                chunk = Mix_QuickLoad_RAW(cvt.buf, static_cast<uint32_t>(cvt.len * cvt.len_ratio));
                if (_sfx.size() > 100) // TODO: make this configurable
                {
                    Mix_FreeChunk(_sfx.begin()->second);
                    _sfx.erase(_sfx.begin());
                }
                _sfx.insert(std::pair<std::string, Mix_Chunk*>(acm->filename(), chunk));
            }
            Mix_PlayChannel(-1, chunk, 0);
        }

        void Mixer::stopSounds()
        {
            Mix_HaltChannel(-1);
        }

        void Mixer::pauseMusic()
        {
            _paused = true;
        }

        void Mixer::resumeMusic()
        {
            _paused = false;
        }

        double Mixer::musicVolume()
        {
            return _musicVolume;
        }

        void Mixer::setMusicVolume(double volume)
        {
            if (volume < 0.0) volume = 0.0;
            else if (volume > 1.0) volume = 1.0;
            _musicVolume = volume;
        }

        std::string &Mixer::lastMusic()
        {
            return _lastMusic;
        }
    }




}
