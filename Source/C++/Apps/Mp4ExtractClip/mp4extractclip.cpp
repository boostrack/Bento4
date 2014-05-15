//
//  extractclip.cpp
//  Bento4
//
//  Created by Michael Rondinelli on 5/14/14.
//
//

/*----------------------------------------------------------------------
 |   includes
 +---------------------------------------------------------------------*/
#include <stdio.h>
#include <stdlib.h>

#include "Ap4.h"

/*----------------------------------------------------------------------
 |   constants
 +---------------------------------------------------------------------*/
#define BANNER "ExtractClip - Version 1.0\n"\
"(Bento4 Version " AP4_VERSION_STRING ")\n"\
"(c) 2014 EyeSee360, Inc."

/*----------------------------------------------------------------------
 |   constants
 +---------------------------------------------------------------------*/

/*----------------------------------------------------------------------
 |   options
 +---------------------------------------------------------------------*/
typedef struct _Options {
    bool no_audio : 1;
    bool no_meta : 1;
    bool no_uuid : 1;
    bool thumbnail_mode : 1;
} Options;

/*----------------------------------------------------------------------
 |   PrintUsageAndExit
 +---------------------------------------------------------------------*/
static void
PrintUsageAndExit()
{
    fprintf(stderr,
            BANNER
            "\n\nusage: mp4extractclip --start <start_time> --end <end_time> <options> <input> <output>\n"
            );
    exit(1);
}


AP4_Track* TrimTrack(AP4_Track *masterTrack, AP4_UI32 firstSample, AP4_UI32 lastSample)
{
    AP4_SyntheticSampleTable* sample_table = new AP4_SyntheticSampleTable();

    // add clones of the sample descriptions to the new sample table
    for (unsigned int i=0; ;i++) {
        AP4_SampleDescription* sample_description = masterTrack->GetSampleDescription(i);
        if (sample_description == NULL) break;
        sample_table->AddSampleDescription(sample_description->Clone());
    }

    // Adjust durations and time offsets
    AP4_UI64 dtsOffset = 0, endTime = 0, mediaDuration = 0, movieDuration = 0;
    AP4_Sample sample;
    if (AP4_SUCCEEDED(masterTrack->GetSample(firstSample, sample))) {
        dtsOffset = sample.GetDts();
    }
    if (AP4_SUCCEEDED(masterTrack->GetSample(lastSample, sample))) {
        endTime = sample.GetDts() + sample.GetDuration();
    }
    mediaDuration = endTime - dtsOffset;
    movieDuration = AP4_ConvertTime(mediaDuration, masterTrack->GetMediaTimeScale(), masterTrack->GetMovieTimeScale());

    for (AP4_Ordinal index = firstSample;
         index <= lastSample && AP4_SUCCEEDED(masterTrack->GetSample(index, sample)); index++) {

        AP4_ByteStream* data_stream;
        data_stream = sample.GetDataStream();
        sample_table->AddSample(*data_stream,
                                sample.GetOffset(),
                                sample.GetSize(),
                                sample.GetDuration(),
                                sample.GetDescriptionIndex(),
                                sample.GetDts() - dtsOffset,
                                sample.GetCtsDelta(),
                                sample.IsSync());
        AP4_RELEASE(data_stream); // release our ref, the table has kept its own ref.
    }

    // create the cloned track
    AP4_Track* clone = new AP4_Track(masterTrack->GetType(),
                                     sample_table,
                                     masterTrack->GetId(),
                                     masterTrack->GetMovieTimeScale(),
                                     movieDuration,
                                     masterTrack->GetMediaTimeScale(),
                                     mediaDuration,
                                     masterTrack->GetTrackLanguage(),
                                     masterTrack->GetWidth(),
                                     masterTrack->GetHeight());

    return clone;
}

AP4_Result SelectExtractionRange(AP4_Movie *srcMovie, AP4_UI64 &startTime, AP4_UI64 &endTime, AP4_UI32 &timescale)
{
    AP4_Result result = AP4_SUCCESS;

    // update and trim tracks
    for (AP4_List<AP4_Track>::Item* track_item = srcMovie->GetTracks().FirstItem();
         track_item;
         track_item = track_item->GetNext()) {

        AP4_Track *track = track_item->GetData();
        AP4_Ordinal startSampleIndex, syncSampleIndex, endSampleIndex;
        AP4_UI64 dts;

        result = track->GetSampleIndexForTimeStamp(startTime, timescale, startSampleIndex);
        if (!AP4_SUCCEEDED(result)) break;

        syncSampleIndex = track->GetNearestSyncSampleIndex(startSampleIndex);
        if (syncSampleIndex != startSampleIndex) {
            AP4_Sample firstSample;
            startSampleIndex = syncSampleIndex;
            track->GetSample(syncSampleIndex, firstSample);
            dts = firstSample.GetDts();
            startTime = dts;
            if (timescale != track->GetMediaTimeScale()) {
                endTime = AP4_ConvertTime(endTime, timescale, track->GetMediaTimeScale());
                timescale = track->GetMediaTimeScale();
            }
        }

        if (endTime < startTime) {
            endSampleIndex = startSampleIndex + 1;
            endTime = startTime;
        } else {
            result = track->GetSampleIndexForTimeStamp(endTime, timescale, endSampleIndex);
            if (!AP4_SUCCEEDED(result)) {
                endSampleIndex = track->GetSampleCount() - 1;
            }
        }

        AP4_Sample lastSample;
        track->GetSample(endSampleIndex, lastSample);
        dts = lastSample.GetDts();
        endTime = dts;
        if (timescale != track->GetMediaTimeScale()) {
            startTime = AP4_ConvertTime(startTime, timescale, track->GetMediaTimeScale());
            timescale = track->GetMediaTimeScale();
        }
        
        result = AP4_SUCCESS;

    }

    return result;
}

AP4_Movie *NewTrimmedMovie(AP4_Movie *srcMovie, AP4_ByteStream& stream, AP4_UI64 startTime, AP4_UI64 endTime, AP4_UI32 timescale, Options &options)
{
    AP4_Result result = AP4_SUCCESS;

    // Get the original tracks
    AP4_Movie *newMovie = new AP4_Movie(srcMovie->GetTimeScale());

    // update and trim tracks
    for (AP4_List<AP4_Track>::Item* track_item = srcMovie->GetTracks().FirstItem();
         track_item;
         track_item = track_item->GetNext()) {

        AP4_Track *track = track_item->GetData();
        AP4_Ordinal startSample, endSample;

        if (options.no_audio && track->GetType() == AP4_Track::TYPE_AUDIO) continue;

        result = track->GetSampleIndexForTimeStamp(startTime, timescale, startSample);
        if (!AP4_SUCCEEDED(result)) break;
        startSample = track->GetNearestSyncSampleIndex(startSample, false);
        result = track->GetSampleIndexForTimeStamp(endTime, timescale, endSample);
        if (!AP4_SUCCEEDED(result)) break;

        AP4_Track *newTrack = TrimTrack(track, startSample, endSample);
        newMovie->AddTrack(newTrack);
    }

    if (!AP4_SUCCEEDED(result)) {
        delete newMovie;
        newMovie = NULL;
    }

    // copy metadata
    if (!options.no_meta) {
        for (AP4_List<AP4_Atom>::Item* atom_item = srcMovie->GetMoovAtom()->GetChildren().FirstItem();
             atom_item; atom_item = atom_item->GetNext()) {

            AP4_Atom *atom = atom_item->GetData();
            switch (atom->GetType()) {
                case AP4_ATOM_TYPE_TRAK:
                case AP4_ATOM_TYPE_MVHD:
                    break;

                default:
                    newMovie->GetMoovAtom()->AddChild(atom->Clone());
            }
        }
    }

    return newMovie;
}

AP4_Movie *NewThumbnailMovie(AP4_Movie *srcMovie, AP4_ByteStream& stream, AP4_UI64 thumbTime, AP4_UI32 timescale)
{
    AP4_Result result = AP4_SUCCESS;

    AP4_Track *track = srcMovie->GetTrack(AP4_Track::TYPE_VIDEO);
    AP4_Ordinal startSample;

    AP4_Movie *newMovie = NULL;

    result = track->GetSampleIndexForTimeStamp(thumbTime, timescale, startSample);

    if (AP4_SUCCEEDED(result)) {
        startSample = track->GetNearestSyncSampleIndex(startSample, false);
        AP4_Track *newTrack = TrimTrack(track, startSample, startSample);
        if (newTrack) {
            newMovie = new AP4_Movie(track->GetMediaTimeScale());
            if (newMovie) {
                newMovie->AddTrack(newTrack);
            }
        }
    }

    return newMovie;
}


/*----------------------------------------------------------------------
 |   main
 +---------------------------------------------------------------------*/
int
main(int argc, char** argv)
{
    if (argc < 4) {
        PrintUsageAndExit();
    }

    // init the variables
    const char*  input_filename    = NULL;
    const char*  output_filename   = NULL;
    AP4_UI64     start_time = 0ull;
    AP4_UI64     end_time = 0ull;
    AP4_UI64     duration_time = 0ull;
    AP4_UI32     timescale = 1000;
    AP4_Result   result;
    Options      opt;

    // parse the command line
    argv++;
    char* arg;
    while ((arg = *argv++)) {
        if (!strcmp(arg, "--start")) {
            arg = *argv++;
            if (arg == NULL) {
                fprintf(stderr, "ERROR: missing argument after --start option\n");
                return 1;
            }
            start_time = strtoull(arg, NULL, 10);
        } else if (!strcmp(arg, "--end")) {
            arg = *argv++;
            if (arg == NULL) {
                fprintf(stderr, "ERROR: missing argument after --end option\n");
                return 1;
            }
            end_time = strtoull(arg, NULL, 10);
        } else if (!strcmp(arg, "--duration")) {
            arg = *argv++;
            if (arg == NULL) {
                fprintf(stderr, "ERROR: missing argument after --end option\n");
                return 1;
            }
            end_time = strtoull(arg, NULL, 10);
        } else if (!strcmp(arg, "--timescale")) {
            arg = *argv++;
            if (arg == NULL) {
                fprintf(stderr, "ERROR: missing argument after --timescale option\n");
                return 1;
            }
            timescale = strtoul(arg, NULL, 10);
        } else if (!strcmp(arg, "--noaudio")) {
            opt.no_audio = true;
        } else if (!strcmp(arg, "--nometa")) {
            opt.no_meta = true;
        } else if (!strcmp(arg, "--nouuid")) {
            opt.no_uuid = true;
        } else if (!strcmp(arg, "--thumb")) {
            opt.no_audio = true;
            opt.no_meta = true;
            opt.no_uuid = false;
            opt.thumbnail_mode = true;
        } else {
            if (input_filename == NULL) {
                input_filename = arg;
            } else if (output_filename == NULL) {
                output_filename = arg;
            } else {
                fprintf(stderr, "ERROR: unexpected argument '%s'\n", arg);
                return 1;
            }
        }
    }

    if (input_filename == NULL) {
        fprintf(stderr, "ERROR: no input specified\n");
        return 1;
    }
    AP4_ByteStream* input_stream = NULL;
    result = AP4_FileByteStream::Create(input_filename,
                                        AP4_FileByteStream::STREAM_MODE_READ,
                                        input_stream);
    if (AP4_FAILED(result)) {
        fprintf(stderr, "ERROR: cannot open input (%d)\n", result);
        return 1;
    }

    if (output_filename == NULL) {
        fprintf(stderr, "ERROR: no output specified\n");
        return 1;
    }
    AP4_ByteStream* output_stream = NULL;
    result = AP4_FileByteStream::Create(output_filename,
                                        AP4_FileByteStream::STREAM_MODE_WRITE,
                                        output_stream);
    if (AP4_FAILED(result)) {
        fprintf(stderr, "ERROR: cannot create/open output (%d)\n", result);
        return 1;
    }

    if (end_time == 0 && duration_time != 0) {
        end_time = start_time + duration_time;
    }

    // parse the input MP4 file (moov only)
    AP4_File input_file(*input_stream, AP4_DefaultAtomFactory::Instance, true);

    // check the file for basic properties
    if (input_file.GetMovie() == NULL) {
        fprintf(stderr, "ERROR: no movie found in the file\n");
        return 1;
    }
    if (input_file.GetMovie()->HasFragments()) {
        fprintf(stderr, "ERROR: file is fragmented\n");
        return 1;
    }

    AP4_Movie *newMovie = NULL;
    if (!opt.thumbnail_mode) {
        SelectExtractionRange(input_file.GetMovie(), start_time, end_time, timescale);
        newMovie = NewTrimmedMovie(input_file.GetMovie(), *input_stream, start_time, end_time, timescale, opt);
    } else {
        newMovie = NewThumbnailMovie(input_file.GetMovie(), *input_stream, start_time, timescale);
    }

    if (newMovie) {
        // create a multimedia file
        AP4_File* file = new AP4_File(newMovie);

        // set the file type
        AP4_UI32 compatible_brands[2] = {
            AP4_FILE_BRAND_ISOM,
            AP4_FILE_BRAND_MP42
        };
        file->SetFileType(AP4_FILE_BRAND_M4A_, 0, compatible_brands, 2);

        if (!opt.no_uuid) {
            // copy xmp
            AP4_Atom *uuidAtom = input_file.GetChild(AP4_ATOM_TYPE_UUID);
            if (uuidAtom) {
                result = file->AddChild(uuidAtom->Clone(), 1);
            }
        }

        // write the file to the output
        AP4_FileWriter::Write(*file, *output_stream);

        delete file;

        fprintf(stdout, "{ \"start\": [%lld, %u], \"duration\": [%lld, %u] }\n",
                start_time, timescale, end_time - start_time, timescale);
    }

    // cleanup and exit
    if (input_stream)  input_stream->Release();
    if (output_stream) output_stream->Release();

    return 0;
}
