#include "logger/logger.hpp"
#include "fs/fs.hpp"
#include "global.hpp"
#include "audio.hpp"
#include "bass_api.hpp"
#include "hook/hook.hpp"
#include "settings/settings.hpp"

#include <cstring>
#include <string>

struct ID3V1_TAG
{
	char id[3];
	char title[30];
	char artist[30];
	char album[30];
	char year[4];
	char comment[30];
	unsigned char genre;
};


static DWORD ReadSyncSafe(const unsigned char* p)
{
	return ((p[0] & 0x7F) << 21) |
		((p[1] & 0x7F) << 14) |
		((p[2] & 0x7F) << 7) |
		(p[3] & 0x7F);
}


static DWORD ReadBE32(const unsigned char* p)
{
	return (DWORD(p[0]) << 24) |
		(DWORD(p[1]) << 16) |
		(DWORD(p[2]) << 8) |
		DWORD(p[3]);
}

static DWORD ReadLE32(const unsigned char* p)
{
	return DWORD(p[0]) |
		(DWORD(p[1]) << 8) |
		(DWORD(p[2]) << 16) |
		(DWORD(p[3]) << 24);
}


static DWORD ReadBE24(const unsigned char* p)
{
	return (DWORD(p[0]) << 16) |
		(DWORD(p[1]) << 8) |
		DWORD(p[2]);
}

static std::string ReadID3Text(
	const unsigned char* data,
	DWORD size)
{
	if (size <= 1)
		return {};


	unsigned char encoding = data[0];

	data++;
	size--;


	// ISO-8859-1 / UTF-8
	if (encoding == 0 || encoding == 3)
	{
		return std::string(
			(char*)data,
			size
		);
	}


	// UTF-16
	if (encoding == 1)
	{
		std::string result;


		for (DWORD i = 0; i + 1 < size; i += 2)
		{
			unsigned short c =
				data[i] |
				(data[i + 1] << 8);


			if (c && c < 128)
				result.push_back((char)c);
		}


		return result;
	}


	return {};
}


static void GetID3v2Tags(
	DWORD stream,
	std::string& title,
	std::string& artist,
	std::string& album)
{
	const unsigned char* data =
		static_cast<const unsigned char*>(
			bass_api::channel_get_tags(
				stream,
				bass_api::tag_id3v2
			)
		);


	if (!data || memcmp(data, "ID3", 3) != 0)
		return;


	DWORD size = ReadSyncSafe(data + 6);
	unsigned char version = data[3];

	size_t offset = 10;


	while (offset + 10 <= size + 10)
	{
		const unsigned char* frame =
			data + offset;


		if (frame[0] == 0)
			break;


		char id[5]{};
		memcpy(id, frame, 4);


		DWORD frameSize =
			(version == 4)
			? ReadSyncSafe(frame + 4)
			: ReadBE32(frame + 4);


		if (frameSize == 0)
			break;


		std::string value =
			ReadID3Text(
				frame + 10,
				frameSize
			);


		if (!value.empty())
		{
			if (strcmp(id, "TIT2") == 0)
			{
				if (title == "N/A")
					title = value;
			}
			else if (strcmp(id, "TPE1") == 0)
			{
				if (artist == "N/A")
					artist = value;
			}
			else if (strcmp(id, "TALB") == 0)
			{
				if (album == "N/A")
					album = value;
			}
		}


		offset += 10 + frameSize;
	}
}

static void GetRIFFTags(
	DWORD stream,
	std::string& title,
	std::string& artist,
	std::string& album)
{
	const char* data =
		static_cast<const char*>(
			bass_api::channel_get_tags(
				stream,
				bass_api::tag_riff_info
			)
		);


	if (!data)
		return;


	const char* p = data;


	while (*p)
	{
		std::string tag = p;


		size_t split =
			tag.find('=');


		// BASS normally returns "key=value" strings,
		// but keep compatibility with raw RIFF INFO too
		if (split != std::string::npos)
		{
			std::string key =
				tag.substr(0, split);

			std::string value =
				tag.substr(split + 1);


			if (_stricmp(key.c_str(), "INAM") == 0)
			{
				if (title == "N/A")
					title = value;
			}
			else if (_stricmp(key.c_str(), "IART") == 0)
			{
				if (artist == "N/A")
					artist = value;
			}
			else if (_stricmp(key.c_str(), "IPRD") == 0)
			{
				if (album == "N/A")
					album = value;
			}
		}
		else
		{
			// fallback for raw null-separated RIFF INFO
			std::string key = tag;

			p += key.size() + 1;

			if (!*p)
				break;


			std::string value = p;


			if (_stricmp(key.c_str(), "INAM") == 0)
			{
				if (title == "N/A")
					title = value;
			}
			else if (_stricmp(key.c_str(), "IART") == 0)
			{
				if (artist == "N/A")
					artist = value;
			}
			else if (_stricmp(key.c_str(), "IPRD") == 0)
			{
				if (album == "N/A")
					album = value;
			}


			p += value.size() + 1;
			continue;
		}


		p += tag.size() + 1;
	}
}


static void GetFLACTags(
	const char* file,
	std::string& title,
	std::string& artist,
	std::string& album)
{
	FILE* fp = fopen(file, "rb");

	if (!fp)
		return;


	unsigned char magic[4];

	fread(
		magic,
		1,
		4,
		fp
	);


	if (memcmp(magic, "fLaC", 4) != 0)
	{
		fclose(fp);
		return;
	}


	while (true)
	{
		unsigned char header[4];

		if (fread(header, 1, 4, fp) != 4)
			break;


		bool last =
			(header[0] & 0x80) != 0;


		unsigned char type =
			header[0] & 0x7F;


		DWORD size =
			ReadBE24(header + 1);


		if (type == 4) // VORBIS_COMMENT
		{
			std::vector<unsigned char> data(size);


			if (fread(data.data(), 1, size, fp) != size)
				break;


			const unsigned char* p =
				data.data();


			DWORD vendorSize =
				ReadLE32(p);


			p += 4 + vendorSize;


			if (p + 4 > data.data() + size)
				break;


			DWORD count =
				ReadLE32(p);


			p += 4;


			for (DWORD i = 0; i < count; i++)
			{
				if (p + 4 > data.data() + size)
					break;


				DWORD len =
					ReadLE32(p);


				p += 4;


				if (p + len > data.data() + size)
					break;


				std::string comment(
					(char*)p,
					len
				);


				p += len;


				size_t eq =
					comment.find('=');


				if (eq == std::string::npos)
					continue;


				std::string key =
					comment.substr(
						0,
						eq
					);


				std::string value =
					comment.substr(
						eq + 1
					);


				if (_stricmp(key.c_str(), "TITLE") == 0)
				{
					if (title == "N/A")
						title = value;
				}
				else if (_stricmp(key.c_str(), "ARTIST") == 0)
				{
					if (artist == "N/A")
						artist = value;
				}
				else if (_stricmp(key.c_str(), "ALBUM") == 0)
				{
					if (album == "N/A")
						album = value;
				}
			}

			break;
		}
		else
		{
			fseek(
				fp,
				size,
				SEEK_CUR
			);
		}


		if (last)
			break;
	}


	fclose(fp);
}

static void GetExtraTags(
	const char* file,
	DWORD stream,
	std::string& title,
	std::string& artist,
	std::string& album)
{
	// ID3v2 (MP3, some FLAC with ID3 blocks)
	GetID3v2Tags(
		stream,
		title,
		artist,
		album
	);


	// RIFF INFO (WAV)
	GetRIFFTags(
		stream,
		title,
		artist,
		album
	);


	// FLAC Vorbis comments
	GetFLACTags(
		file,
		title,
		artist,
		album
	);
}


void play_file(const char* file, int channel)
{
	audio::chan[channel] =
		static_cast<std::int32_t>(
			bass_api::stream_create_file(file)
		);


	audio::apply_current_context_volume();


	if (audio::chan[channel] == 0 ||
		!bass_api::channel_play(
			audio::chan[channel],
			false))
	{
		return;
	}


	audio::playing = true;


	std::string title = "N/A";
	std::string artist = "N/A";
	std::string album = "N/A";


	DWORD stream =
		static_cast<DWORD>(
			audio::chan[channel]
		);


	// Existing ID3v1 support
	const ID3V1_TAG* tag =
		static_cast<const ID3V1_TAG*>(
			bass_api::channel_get_tags(
				stream,
				bass_api::tag_id3
			)
		);


	if (tag &&
		memcmp(tag->id, "TAG", 3) == 0)
	{
		if (tag->title[0])
			title.assign(tag->title, 30);


		if (tag->artist[0])
			artist.assign(tag->artist, 30);
	}


	// ID3v2 / RIFF / FLAC
	GetExtraTags(
		file,
		stream,
		title,
		artist,
		album
	);


	if (album == "N/A")
		album = audio::playlist_name;


	// Filename fallback
	if (title == "N/A" ||
		artist == "N/A")
	{
		std::string temp = file;


		if (temp.size() >
			audio::playlist_dir.size() + 1)
		{
			temp.erase(
				0,
				audio::playlist_dir.size() + 1
			);
		}


		size_t dot =
			temp.rfind('.');


		if (dot != std::string::npos)
			temp.erase(dot);


		size_t dash =
			temp.find('-');


		if (dash != std::string::npos)
		{
			if (artist == "N/A")
			{
				artist =
					temp.substr(
						0,
						dash
					);
			}


			if (title == "N/A")
			{
				title =
					temp.substr(
						dash + 1
					);
			}
		}
		else if (title == "N/A")
		{
			title = temp;
		}


		logger::trim(title);
		logger::trim(artist);
	}


	audio::currently_playing.title = title;
	audio::currently_playing.artist = artist;
	audio::currently_playing.where = album;


	audio::request_current_chyron();
}
