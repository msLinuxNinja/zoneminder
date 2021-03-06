//
// ZoneMinder Event Class Implementation, $Date$, $Revision$
// Copyright (C) 2001-2008 Philip Coombes
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
//

#include <fcntl.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/un.h>
#include <sys/uio.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <getopt.h>
#include <arpa/inet.h>
#include <glob.h>
#include <cinttypes>

#include "zm.h"
#include "zm_db.h"
#include "zm_time.h"
#include "zm_signal.h"
#include "zm_event.h"
#include "zm_monitor.h"

//#define USE_PREPARED_SQL 1

const char * Event::frame_type_names[3] = { "Normal", "Bulk", "Alarm" };
char frame_insert_sql[ZM_SQL_LGE_BUFSIZ] = "INSERT INTO `Frames` (`EventId`, `FrameId`, `Type`, `TimeStamp`, `Delta`, `Score`) VALUES ";

int Event::pre_alarm_count = 0;

Event::PreAlarmData Event::pre_alarm_data[MAX_PRE_ALARM_FRAMES] = { { 0 } };

Event::Event(
    Monitor *p_monitor,
    struct timeval p_start_time,
    const std::string &p_cause,
    const StringSetMap &p_noteSetMap,
    bool p_videoEvent ) :
  monitor(p_monitor),
  start_time(p_start_time),
  cause(p_cause),
  noteSetMap(p_noteSetMap),
  videoEvent(p_videoEvent),
  videowriter(nullptr)
{

  std::string notes;
  createNotes(notes);

  struct timeval now;
  gettimeofday(&now, 0);

  bool untimedEvent = false;
  if ( !start_time.tv_sec ) {
    untimedEvent = true;
    start_time = now;
  } else if ( start_time.tv_sec > now.tv_sec ) {
    Error(
        "StartTime in the future %u.%u > %u.%u",
        start_time.tv_sec, start_time.tv_usec, now.tv_sec, now.tv_usec
        );
    start_time = now;
  }

  Storage * storage = monitor->getStorage();
  scheme = storage->Scheme();

  unsigned int state_id = 0;
  zmDbRow dbrow;
  if ( dbrow.fetch("SELECT Id FROM States WHERE IsActive=1") ) {
    state_id = atoi(dbrow[0]);
  }

  char sql[ZM_SQL_MED_BUFSIZ];
  struct tm *stime = localtime(&start_time.tv_sec);
  snprintf(sql, sizeof(sql), "INSERT INTO Events "
      "( MonitorId, StorageId, Name, StartTime, Width, Height, Cause, Notes, StateId, Orientation, Videoed, DefaultVideo, SaveJPEGs, Scheme )"
     " VALUES ( %u, %u, 'New Event', from_unixtime( %ld ), %u, %u, '%s', '%s', %u, %d, %d, '%s', %d, '%s' )",
      monitor->Id(), 
      storage->Id(),
      start_time.tv_sec,
      monitor->Width(),
      monitor->Height(),
      cause.c_str(),
      notes.c_str(), 
      state_id,
      monitor->getOrientation(),
      videoEvent,
			( monitor->GetOptVideoWriter() != 0 ? "video.mp4" : "" ),
      monitor->GetOptSaveJPEGs(),
      storage->SchemeString().c_str()
      );
  db_mutex.lock();
  if ( mysql_query(&dbconn, sql) ) {
    db_mutex.unlock();
    Error("Can't insert event: %s. sql was (%s)", mysql_error(&dbconn), sql);
    return;
  } else {
    Debug(2, "Created new event with %s", sql);
  }
  id = mysql_insert_id(&dbconn);

  db_mutex.unlock();
  if ( untimedEvent ) {
    Warning("Event %d has zero time, setting to current", id);
  }
  end_time.tv_sec = 0;
  frames = 0;
  alarm_frames = 0;
  tot_score = 0;
  max_score = 0;
  alarm_frame_written = false;

  std::string id_file;

  path = stringtf("%s/%d", storage->Path(), monitor->Id());
  // Try to make the Monitor Dir.  Normally this would exist, but in odd cases might not.
  if ( mkdir(path.c_str(), 0755) ) {
    if ( errno != EEXIST )
      Error("Can't mkdir %s: %s", path.c_str(), strerror(errno));
  }

  if ( storage->Scheme() == Storage::DEEP ) {

    int dt_parts[6];
    dt_parts[0] = stime->tm_year-100;
    dt_parts[1] = stime->tm_mon+1;
    dt_parts[2] = stime->tm_mday;
    dt_parts[3] = stime->tm_hour;
    dt_parts[4] = stime->tm_min;
    dt_parts[5] = stime->tm_sec;

    std::string date_path;
    std::string time_path;

    for ( unsigned int i = 0; i < sizeof(dt_parts)/sizeof(*dt_parts); i++ ) {
      path += stringtf("/%02d", dt_parts[i]);

      if ( mkdir(path.c_str(), 0755) ) {
        // FIXME This should not be fatal.  Should probably move to a different storage area.
        if ( errno != EEXIST )
          Error("Can't mkdir %s: %s", path.c_str(), strerror(errno));
      }
      if ( i == 2 )
				date_path = path;
    }
		time_path = stringtf("%02d/%02d/%02d", stime->tm_hour, stime->tm_min, stime->tm_sec);

    // Create event id symlink
    id_file = stringtf("%s/.%" PRIu64, date_path.c_str(), id);
    if ( symlink(time_path.c_str(), id_file.c_str()) < 0 )
      Error("Can't symlink %s -> %s: %s", id_file.c_str(), time_path.c_str(), strerror(errno));
  } else if ( storage->Scheme() == Storage::MEDIUM ) {
    path += stringtf("/%04d-%02d-%02d",
        stime->tm_year+1900, stime->tm_mon+1, stime->tm_mday
        );
    if ( mkdir(path.c_str(), 0755) ) {
      if ( errno != EEXIST )
        Error("Can't mkdir %s: %s", path.c_str(), strerror(errno));
    }
    path += stringtf("/%" PRIu64, id);
    if ( mkdir(path.c_str(), 0755) ) {
      if ( errno != EEXIST )
        Error("Can't mkdir %s: %s", path.c_str(), strerror(errno));
    }
  } else {
    path += stringtf("/%" PRIu64, id);
    if ( mkdir(path.c_str(), 0755) ) {
      if ( errno != EEXIST )
        Error("Can't mkdir %s: %s", path.c_str(), strerror(errno));
    }

    // Create empty id tag file
    id_file = stringtf("%s/.%" PRIu64, path.c_str(), id);
    if ( FILE *id_fp = fopen(id_file.c_str(), "w") ) {
      fclose(id_fp);
    } else {
      Error("Can't fopen %s: %s", id_file.c_str(), strerror(errno));
		}
  } // deep storage or not

  last_db_frame = 0;

  video_name = "";

  snapshot_file = path + "/snapshot.jpg";
  alarm_file = path + "/alarm.jpg";

  /* Save as video */

  if ( monitor->GetOptVideoWriter() != 0 ) {
    video_name = stringtf("%" PRIu64 "-%s", id, "video.mp4");
    snprintf(sql, sizeof(sql), "UPDATE Events SET DefaultVideo = '%s' WHERE Id=%" PRIu64, video_name.c_str(), id);
    if ( mysql_query(&dbconn, sql) ) {
      db_mutex.unlock();
      Error("Can't update event: %s. sql was (%s)", mysql_error(&dbconn), sql);
      return;
    }
    video_file = path + "/" + video_name;
			Debug(1, "Writing video file to %s", video_file.c_str());

    /* X264 MP4 video writer */
    if ( monitor->GetOptVideoWriter() == Monitor::X264ENCODE ) {
#if ZM_HAVE_VIDEOWRITER_X264MP4
      videowriter = new X264MP4Writer(video_file.c_str(),
          monitor->Width(),
          monitor->Height(),
          monitor->Colours(),
          monitor->SubpixelOrder(),
					monitor->GetOptEncoderParamsVec());
#else
      Error("ZoneMinder was not compiled with the X264 MP4 video writer, check dependencies (x264 and mp4v2)");
#endif

			if ( videowriter != nullptr ) {
				/* Open the video stream */
				int nRet = videowriter->Open();
				if ( nRet != 0 ) {
					Error("Failed opening video stream");
					delete videowriter;
					videowriter = nullptr;
				}
			}
		}
  } else {
    /* No video object */
    videowriter = nullptr;
  }
} // Event::Event( Monitor *p_monitor, struct timeval p_start_time, const std::string &p_cause, const StringSetMap &p_noteSetMap, bool p_videoEvent )

Event::~Event() {
  // We close the videowriter first, because if we finish the event, we might try to view the file, but we aren't done writing it yet.

  /* Close the video file */
  if ( videowriter != nullptr ) {
    int nRet = videowriter->Close();
    if ( nRet != 0 ) {
      Error("Failed closing video stream");
    }
    delete videowriter;
    videowriter = nullptr;
  }

  struct DeltaTimeval delta_time;
  DELTA_TIMEVAL(delta_time, end_time, start_time, DT_PREC_2);
  Debug(2, "start_time:%d.%d end_time%d.%d", start_time.tv_sec, start_time.tv_usec, end_time.tv_sec, end_time.tv_usec);

#if 0  // This closing frame has no image. There is no point in adding a db record for it, I think. ICON
  if ( frames > last_db_frame ) {
    frames ++;
    Debug(1, "Adding closing frame %d to DB", frames);
    frame_data.push(new Frame(id, frames, NORMAL, end_time, delta_time, 0));
  }
#endif
  if ( frame_data.size() )
    WriteDbFrames();

  // update frame deltas to refer to video start time which may be a few frames before event start
  struct timeval video_offset = {0};
  struct timeval video_start_time  = monitor->GetVideoWriterStartTime();
  if ( video_start_time.tv_sec > 0 ) {
     timersub(&video_start_time, &start_time, &video_offset);
     Debug(1, "Updating frames delta by %d sec %d usec",
           video_offset.tv_sec, video_offset.tv_usec);
     UpdateFramesDelta(video_offset.tv_sec + video_offset.tv_usec*1e-6);
  } else {
     Debug(3, "Video start_time %d sec %d usec not valid -- frame deltas not updated",
           video_start_time.tv_sec, video_start_time.tv_usec);
  }

  // Should not be static because we might be multi-threaded
  char sql[ZM_SQL_LGE_BUFSIZ];
  snprintf(sql, sizeof(sql),
      "UPDATE Events SET Name='%s%" PRIu64 "', EndTime = from_unixtime(%ld), Length = %s%ld.%02ld, Frames = %d, AlarmFrames = %d, TotScore = %d, AvgScore = %d, MaxScore = %d WHERE Id = %" PRIu64 " AND Name='New Event'",
      monitor->EventPrefix(), id, end_time.tv_sec,
      delta_time.positive?"":"-", delta_time.sec, delta_time.fsec,
      frames, alarm_frames,
      tot_score, (int)(alarm_frames?(tot_score/alarm_frames):0), max_score,
      id);
  db_mutex.lock();
  while ( mysql_query(&dbconn, sql) && !zm_terminate ) {
    db_mutex.unlock();
    Error("Can't update event: %s reason: %s", sql, mysql_error(&dbconn));
    sleep(1);
    db_mutex.lock();
  }
  if ( !mysql_affected_rows(&dbconn) ) {
    // Name might have been changed during recording, so just do the update without changing the name.
    snprintf(sql, sizeof(sql),
        "UPDATE Events SET EndTime = from_unixtime(%ld), Length = %s%ld.%02ld, Frames = %d, AlarmFrames = %d, TotScore = %d, AvgScore = %d, MaxScore = %d WHERE Id = %" PRIu64,
        end_time.tv_sec,
        delta_time.positive?"":"-", delta_time.sec, delta_time.fsec,
        frames, alarm_frames,
        tot_score, (int)(alarm_frames?(tot_score/alarm_frames):0), max_score,
        id);
    while ( mysql_query(&dbconn, sql) && !zm_terminate ) {
      db_mutex.unlock();
      Error("Can't update event: %s reason: %s", sql, mysql_error(&dbconn));
      sleep(1);
      db_mutex.lock();
    }
  }  // end if no changed rows due to Name change during recording
  db_mutex.unlock();
}  // Event::~Event()

void Event::createNotes(std::string &notes) {
  notes.clear();
  for ( StringSetMap::const_iterator mapIter = noteSetMap.begin(); mapIter != noteSetMap.end(); ++mapIter ) {
    notes += mapIter->first;
    notes += ": ";
    const StringSet &stringSet = mapIter->second;
    for ( StringSet::const_iterator setIter = stringSet.begin(); setIter != stringSet.end(); ++setIter ) {
      if ( setIter != stringSet.begin() )
        notes += ", ";
      notes += *setIter;
    }
  }
}  // void Event::createNotes(std::string &notes)

bool Event::WriteFrameImage(
    Image *image,
    struct timeval timestamp,
    const char *event_file,
    bool alarm_frame) const {

  int thisquality = 
    (alarm_frame && (config.jpeg_alarm_file_quality > config.jpeg_file_quality)) ?
    config.jpeg_alarm_file_quality : 0;   // quality to use, zero is default

  bool rc;

  if ( !config.timestamp_on_capture ) {
    // stash the image we plan to use in another pointer regardless if timestamped.
    // exif is only timestamp at present this switches on or off for write
    Image *ts_image = new Image(*image);
    monitor->TimestampImage(ts_image, &timestamp);
    rc = ts_image->WriteJpeg(event_file, thisquality,
        (monitor->Exif() ? timestamp : (timeval){0,0}));
    delete(ts_image);
  } else {
    rc = image->WriteJpeg(event_file, thisquality,
        (monitor->Exif() ? timestamp : (timeval){0,0}));
  }

  return rc;
}

bool Event::WriteFrameVideo(
    const Image *image,
    const struct timeval timestamp,
    VideoWriter* videow) {
  const Image* frameimg = image;
  Image ts_image;

  /* Checking for invalid parameters */
  if ( videow == nullptr ) {
    Error("NULL Video object");
    return false;
  }

  /* If the image does not contain a timestamp, add the timestamp */
  if ( !config.timestamp_on_capture ) {
    ts_image = *image;
    monitor->TimestampImage(&ts_image, &timestamp);
    frameimg = &ts_image;
  }

  /* Calculate delta time */
  struct DeltaTimeval delta_time3;
  DELTA_TIMEVAL(delta_time3, timestamp, start_time, DT_PREC_3);
  unsigned int timeMS = (delta_time3.sec * delta_time3.prec) + delta_time3.fsec;

  /* Encode and write the frame */
  if ( videowriter->Encode(frameimg, timeMS) != 0 ) {
    Error("Failed encoding video frame");
  }

  return true;
}  // bool Event::WriteFrameVideo

void Event::updateNotes(const StringSetMap &newNoteSetMap) {
  bool update = false;

  //Info( "Checking notes, %d <> %d", noteSetMap.size(), newNoteSetMap.size() );
  if ( newNoteSetMap.size() > 0 ) {
    if ( noteSetMap.size() == 0 ) {
      noteSetMap = newNoteSetMap;
      update = true;
    } else {
      for ( StringSetMap::const_iterator newNoteSetMapIter = newNoteSetMap.begin();
          newNoteSetMapIter != newNoteSetMap.end();
          ++newNoteSetMapIter ) {
        const std::string &newNoteGroup = newNoteSetMapIter->first;
        const StringSet &newNoteSet = newNoteSetMapIter->second;
        //Info( "Got %d new strings", newNoteSet.size() );
        if ( newNoteSet.size() > 0 ) {
          StringSetMap::iterator noteSetMapIter = noteSetMap.find(newNoteGroup);
          if ( noteSetMapIter == noteSetMap.end() ) {
            //Info( "Can't find note group %s, copying %d strings", newNoteGroup.c_str(), newNoteSet.size() );
            noteSetMap.insert(StringSetMap::value_type(newNoteGroup, newNoteSet));
            update = true;
          } else {
            StringSet &noteSet = noteSetMapIter->second;
            //Info( "Found note group %s, got %d strings", newNoteGroup.c_str(), newNoteSet.size() );
            for ( StringSet::const_iterator newNoteSetIter = newNoteSet.begin();
                newNoteSetIter != newNoteSet.end();
                ++newNoteSetIter ) {
              const std::string &newNote = *newNoteSetIter;
              StringSet::iterator noteSetIter = noteSet.find(newNote);
              if ( noteSetIter == noteSet.end() ) {
                noteSet.insert(newNote);
                update = true;
              }
            } // end for
          } // end if ( noteSetMap.size() == 0
        } // end if newNoteSetupMap.size() > 0
      } // end foreach newNoteSetMap
    } // end if have old notes
  } // end if have new notes

  if ( update ) {
    std::string notes;
    createNotes(notes);

    Debug(2, "Updating notes for event %d, '%s'", id, notes.c_str());
    static char sql[ZM_SQL_LGE_BUFSIZ];
#if USE_PREPARED_SQL
    static MYSQL_STMT *stmt = 0;

    char notesStr[ZM_SQL_MED_BUFSIZ] = "";
    unsigned long notesLen = 0;

    if ( !stmt ) {
      const char *sql = "UPDATE `Events` SET `Notes` = ? WHERE `Id` = ?";

      stmt = mysql_stmt_init(&dbconn);
      if ( mysql_stmt_prepare(stmt, sql, strlen(sql)) ) {
        Fatal("Unable to prepare sql '%s': %s", sql, mysql_stmt_error(stmt));
      }

      /* Get the parameter count from the statement */
      if ( mysql_stmt_param_count(stmt) != 2 ) {
        Error("Unexpected parameter count %ld in sql '%s'", mysql_stmt_param_count(stmt), sql);
      }

      MYSQL_BIND  bind[2];
      memset(bind, 0, sizeof(bind));

      /* STRING PARAM */
      bind[0].buffer_type = MYSQL_TYPE_STRING;
      bind[0].buffer = (char *)notesStr;
      bind[0].buffer_length = sizeof(notesStr);
      bind[0].is_null = 0;
      bind[0].length = &notesLen;

      bind[1].buffer_type= MYSQL_TYPE_LONG;
      bind[1].buffer= (char *)&id;
      bind[1].is_null= 0;
      bind[1].length= 0;

      /* Bind the buffers */
      if ( mysql_stmt_bind_param(stmt, bind) ) {
        Error("Unable to bind sql '%s': %s", sql, mysql_stmt_error(stmt));
      }
    }

    strncpy(notesStr, notes.c_str(), sizeof(notesStr));

    if ( mysql_stmt_execute(stmt) ) {
      Error("Unable to execute sql '%s': %s", sql, mysql_stmt_error(stmt));
    }
#else
    static char escapedNotes[ZM_SQL_MED_BUFSIZ];

    mysql_real_escape_string(&dbconn, escapedNotes, notes.c_str(), notes.length());

    snprintf(sql, sizeof(sql), "UPDATE `Events` SET `Notes` = '%s' WHERE `Id` = %" PRIu64, escapedNotes, id);
    db_mutex.lock();
    if ( mysql_query(&dbconn, sql) ) {
      Error("Can't insert event: %s", mysql_error(&dbconn));
    }
    db_mutex.unlock();
#endif
  }  // end if update
}  // void Event::updateNotes(const StringSetMap &newNoteSetMap)

void Event::AddFrames(int n_frames, Image **images, struct timeval **timestamps) {
  for (int i = 0; i < n_frames; i += ZM_SQL_BATCH_SIZE) {
    AddFramesInternal(n_frames, i, images, timestamps);
  }
}

void Event::AddFramesInternal(int n_frames, int start_frame, Image **images, struct timeval **timestamps) {
  char *frame_insert_values = (char *)&frame_insert_sql + 90; // 90 == strlen(frame_insert_sql); 
  //static char sql[ZM_SQL_LGE_BUFSIZ];
  //strncpy(sql, "INSERT INTO `Frames` (`EventId`, `FrameId`, `TimeStamp`, `Delta`) VALUES ", sizeof(sql));
  int frameCount = 0;
  for ( int i = start_frame; i < n_frames && i - start_frame < ZM_SQL_BATCH_SIZE; i++ ) {
    if ( timestamps[i]->tv_sec <= 0 ) {
      Debug(1, "Not adding pre-capture frame %d, zero or less than 0 timestamp", i);
      continue;
    }

    frames++;

    if ( monitor->GetOptSaveJPEGs() & 1 ) {
			std::string event_file = stringtf(staticConfig.capture_file_format, path.c_str(), frames);
      Debug(1, "Writing pre-capture frame %d", frames);
      WriteFrameImage(images[i], *(timestamps[i]), event_file.c_str());
    }
    //If this is the first frame, we should add a thumbnail to the event directory
    // ICON: We are working through the pre-event frames so this snapshot won't 
    // neccessarily be of the motion.  But some events are less than 10 frames, 
    // so I am changing this to 1, but we should overwrite it later with a better snapshot.
    if ( frames == 1 ) {
      WriteFrameImage(images[i], *(timestamps[i]), snapshot_file.c_str());
    }

    if ( videowriter != nullptr ) {
      WriteFrameVideo(images[i], *(timestamps[i]), videowriter);
    }

    struct DeltaTimeval delta_time;
    DELTA_TIMEVAL(delta_time, *(timestamps[i]), start_time, DT_PREC_2);
    // Delta is Decimal(8,2) so 6 integer digits and 2 decimal digits
    if ( delta_time.sec > 999999 ) {
      Warning("Invalid delta_time from_unixtime(%ld), %s%ld.%02ld", 
           timestamps[i]->tv_sec,
           (delta_time.positive?"":"-"),
           delta_time.sec,
           delta_time.fsec);
        delta_time.sec = 0;
    }

    frame_insert_values += snprintf(frame_insert_values,
        sizeof(frame_insert_sql)-(frame_insert_values-(char *)&frame_insert_sql),
        "\n( %" PRIu64 ", %d, 'Normal', from_unixtime(%ld), %s%ld.%02ld, 0 ),",
        id, frames, timestamps[i]->tv_sec, delta_time.positive?"":"-", delta_time.sec, delta_time.fsec);

    frameCount++;
  } // end foreach frame

  if ( frameCount ) {
    *(frame_insert_values-1) = '\0';
    db_mutex.lock();
    int rc = mysql_query(&dbconn, frame_insert_sql);
    db_mutex.unlock();
    if ( rc ) {
      Error("Can't insert frames: %s, sql was (%s)", mysql_error(&dbconn), frame_insert_sql);
    } else {
      Debug(1, "INSERT %d/%d frames sql %s", frameCount, n_frames, frame_insert_sql);
    }
    last_db_frame = frames;
  } else {
    Debug(1, "No valid pre-capture frames to add");
  }
}  // void Event::AddFramesInternal(int n_frames, int start_frame, Image **images, struct timeval **timestamps)

void Event::WriteDbFrames() {
  char *frame_insert_values_ptr = (char *)&frame_insert_sql + 90; // 90 == strlen(frame_insert_sql); 
  Debug(1, "Inserting %d frames", frame_data.size());
  while ( frame_data.size() ) {
    Frame *frame = frame_data.front();
    frame_data.pop();
    frame_insert_values_ptr += snprintf(frame_insert_values_ptr,
        sizeof(frame_insert_sql)-(frame_insert_values_ptr-(char *)&frame_insert_sql),
        "\n( %" PRIu64 ", %d, '%s', from_unixtime( %ld ), %s%ld.%02ld, %d ),",
        id, frame->frame_id,
        frame_type_names[frame->type],
        frame->timestamp.tv_sec,
        frame->delta.positive?"":"-",
        frame->delta.sec,
        frame->delta.fsec,
        frame->score);
    delete frame;
  }
  *(frame_insert_values_ptr-1) = '\0'; // The -1 is for the extra , added for values above
  db_mutex.lock();
  Debug(1, "SQL: %s", frame_insert_sql);
  int rc = mysql_query(&dbconn, frame_insert_sql);
  db_mutex.unlock();

  if ( rc ) {
    Error("Can't insert frames: %s, sql was %s", mysql_error(&dbconn), frame_insert_sql);
    return;
  } else {
    Debug(1, "INSERT FRAMES: sql was %s", frame_insert_sql);
  }
} // end void Event::WriteDbFrames()

// Subtract an offset time from frames deltas to match with video start time
void Event::UpdateFramesDelta(double offset) {
  char sql[ZM_SQL_MED_BUFSIZ];

  if ( offset == 0.0 ) return;
  // the table is set to auto update timestamp so we force it to keep current value
  snprintf(sql, sizeof(sql),
    "UPDATE Frames SET timestamp = timestamp, Delta = Delta - (%.4f) WHERE EventId = %" PRIu64,
    offset, id);

  db_mutex.lock();
  if ( mysql_query(&dbconn, sql) ) {
    db_mutex.unlock();
    Error("Can't update frames: %s, sql was %s", mysql_error(&dbconn), sql);
    return;
  }
  db_mutex.unlock();
  Info("Updating frames delta by %0.2f sec to match video file", offset);
}

void Event::AddFrame(Image *image, struct timeval timestamp, int score, Image *alarm_image) {
  if ( !timestamp.tv_sec ) {
    Debug(1, "Not adding new frame, zero timestamp");
    return;
  }

  frames++;
  bool write_to_db = false;
  FrameType frame_type = score>0?ALARM:(score<0?BULK:NORMAL);
  // < 0 means no motion detection is being done.
  if ( score < 0 )
    score = 0;

  if ( monitor->GetOptSaveJPEGs() & 1 ) {
    std::string event_file = stringtf(staticConfig.capture_file_format, path.c_str(), frames);
    Debug(1, "Writing capture frame %d to %s", frames, event_file.c_str());
    if ( !WriteFrameImage(image, timestamp, event_file.c_str()) ) {
      Error("Failed to write frame image");
    }
  }

  // If this is the first frame, we should add a thumbnail to the event directory
  if ( (frames == 1) || (score > (int)max_score) ) {
    write_to_db = true; // web ui might show this as thumbnail, so db needs to know about it.
    WriteFrameImage(image, timestamp, snapshot_file.c_str());
  }

  // We are writing an Alarm frame
  if ( frame_type == ALARM ) {
    // The first frame with a score will be the frame that alarmed the event
    if ( !alarm_frame_written ) {
      write_to_db = true; // OD processing will need it, so the db needs to know about it
      alarm_frame_written = true;
      WriteFrameImage(image, timestamp, alarm_file.c_str());
    }
    alarm_frames++;

    tot_score += score;
    if ( score > (int)max_score )
      max_score = score;

    if ( alarm_image ) {
      if ( monitor->GetOptSaveJPEGs() & 2 ) {
        std::string event_file = stringtf(staticConfig.analyse_file_format, path.c_str(), frames);
        Debug(1, "Writing analysis frame %d", frames);
        if ( ! WriteFrameImage(alarm_image, timestamp, event_file.c_str(), true) ) {
          Error("Failed to write analysis frame image");
        }
      }
    }
  } // end if frame_type == ALARM

  if ( videowriter != nullptr ) {
    WriteFrameVideo(image, timestamp, videowriter);
  }

  struct DeltaTimeval delta_time;
  DELTA_TIMEVAL(delta_time, timestamp, start_time, DT_PREC_2);
  Debug(1, "Frame delta is %d.%d - %d.%d = %d.%d", 
      start_time.tv_sec, start_time.tv_usec, timestamp.tv_sec, timestamp.tv_usec, delta_time.sec, delta_time.fsec);

  bool db_frame = ( frame_type != BULK ) || (frames==1) || ((frames%config.bulk_frame_interval)==0) ;
  if ( db_frame ) {

    // The idea is to write out 1/sec
    frame_data.push(new Frame(id, frames, frame_type, timestamp, delta_time, score));
    if ( write_to_db or ( monitor->get_fps() and (frame_data.size() > monitor->get_fps())) or frame_type==BULK ) {
      Debug(1, "Adding %d frames to DB because write_to_db:%d or frames > analysis fps %f or BULK",
					frame_data.size(), write_to_db, monitor->get_fps());
      WriteDbFrames();
      last_db_frame = frames;

      static char sql[ZM_SQL_MED_BUFSIZ];
      snprintf(sql, sizeof(sql), 
          "UPDATE Events SET Length = %s%ld.%02ld, Frames = %d, AlarmFrames = %d, TotScore = %d, AvgScore = %d, MaxScore = %d WHERE Id = %" PRIu64, 
          ( delta_time.positive?"":"-" ),
          delta_time.sec, delta_time.fsec,
          frames, 
          alarm_frames,
          tot_score,
          (int)(alarm_frames?(tot_score/alarm_frames):0),
          max_score,
          id
          );
      db_mutex.lock();
      while ( mysql_query(&dbconn, sql) && !zm_terminate ) {
        Error("Can't update event: %s", mysql_error(&dbconn));
        db_mutex.unlock();
        sleep(1);
        db_mutex.lock();
      }
      db_mutex.unlock();
    } // end if frame_type == BULK
  } // end if db_frame

  end_time = timestamp;
}  // end void Event::AddFrame(Image *image, struct timeval timestamp, int score, Image *alarm_image)
