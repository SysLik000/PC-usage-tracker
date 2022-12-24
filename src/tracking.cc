#include "timetracker.hpp"
#include "./ui_timetracker.h"
#include <winuser.h>
#include <sysinfoapi.h>

#define USER_IDLING_DELAY 60

void TimeTracker::updateApplicationAtWorkUsage(QString name, std::chrono::seconds time)
{
  QSqlQuery checkAppWork;
  checkAppWork.prepare("SELECT name FROM ApplicationWorktimeUsage WHERE day=DATE('now', 'localtime') AND name=:appName");
  checkAppWork.bindValue(":appName", name);
  checkAppWork.exec();
  if (!checkAppWork.next())
    {
      QSqlQuery insertQueryWork;
      insertQueryWork.prepare("INSERT OR IGNORE INTO ApplicationWorktimeUsage(name, usage,day,last) VALUES(:appName, 0, DATE('now', 'localtime'),DATETIME('now', 'localtime'))");
      insertQueryWork.bindValue(":appName", name);
      insertQueryWork.exec();
    }
  QSqlQuery updateQueryWork;

  updateQueryWork.prepare("UPDATE ApplicationWorktimeUsage SET usage = usage + :timeIncrement, last=DATETIME('now', 'localtime') WHERE day = DATE('now', 'localtime') AND name=:appName");
  updateQueryWork.bindValue(":appName", name);
  updateQueryWork.bindValue(":timeIncrement", time.count());
  updateQueryWork.exec();
}

void TimeTracker::updateAppStats(QString name, std::chrono::seconds time)
{
  appUsageModel->select();

  QSqlQuery checkApp;
  checkApp.prepare("SELECT name FROM ApplicationUsage WHERE name=:appName AND day=DATE('now', 'localtime')");
  checkApp.bindValue(":appName", name);
  checkApp.exec();

  QSqlQuery insertQuery;
  if (!checkApp.next())
    {
      insertQuery.prepare("INSERT OR IGNORE INTO ApplicationUsage(name, usage,day,last) VALUES(:appName, 0, DATE('now', 'localtime'),DATETIME('now', 'localtime'))");
      insertQuery.bindValue(":appName", name);
      if (!insertQuery.exec())
        {
          qDebug("Failure");
        }
    }
  else
    {
      insertQuery.prepare("UPDATE ApplicationUsage SET usage = usage + :timeIncrement, last=DATETIME('now', 'localtime') WHERE day = DATE('now', 'localtime') AND name=:appName");
      insertQuery.bindValue(":appName", name);
      insertQuery.bindValue(":timeIncrement", time.count());
      insertQuery.exec();
      bool isWorkShift = false;
      QDateTime curTime = QDateTime::currentDateTime();
      if (curTime.time() >= shiftStart_ && curTime.time() <= shiftEnd_)
        {
          isWorkShift = true;
        }
      if (isWorkShift)
        {
          updateApplicationAtWorkUsage(name, time);
        }
    }

  appUsageModel->submitAll();
}

void TimeTracker::onNewDayAction()
{
  qDebug("New day!");
  // it's a new day
  //! FIXME: this is a shitty temp solution
  //! BUG: https://github.com/Pugnator/ApplicationTimeTracker/issues/1

  updateDailyStats();

  currentSession_ = QDate::currentDate();
  daylyLoggedOnTime_ = 0s;
  daylyLoggedOffTime_ = 0s;
  daylyIdlingTime_ = 0s;
  activityTimer->reset();
  logonTimer->reset();
  appModelSetup(showWorkShiftStatsOnly_ ? "ApplicationWorktimeUsage" : "ApplicationUsage");
}

bool TimeTracker::lockSystem()
{
  qDebug("You have to rest");
  isHaveToRest_ = true;
  if(LockWorkStation())
  {
    qDebug("Locked the workstation");
    return true;
  }
  return false;
}

void TimeTracker::onTimerTick(QString name, std::chrono::seconds time)
{  
  if (QDate::currentDate() > currentSession_)
    {
      onNewDayAction();
    }

  if (isSystemLocked_)
    {
      daylyLoggedOffTime_++;
      if (isHaveToRest_)
        {
          timeUserHaveToRest_--;
          if(timeUserHaveToRest_ <= 0s)
            {
              qDebug() << "Rest finished";
              isHaveToRest_ = false;
              timeLeftToLock_ = maxWorkTimeInRow_;
              timeEndWarningShown_ = false;
              setRestTimer();
            }
        }
      else
        {

          if(timeLeftToLock_ < maxWorkTimeInRow_)
            {
              timeLeftToLock_++;
            }
        }
      return;
    }

  daylyLoggedOnTime_++;
  if (!trackingEnabled_)
    {
      return;
    }

  if(restControlEnabled_ && !isHaveToRest_ && timeLeftToLock_ == 0s)
    {
      lockSystem();
    }
  else if (isHaveToRest_)
    {
      qDebug("Ignoring rest");
    }

  if(restControlEnabled_ && timeLeftToLock_ > 0s)
    {
      timeLeftToLock_--;
    }


  if(restControlEnabled_ && timeLeftToLock_  < 350s)
    {
      timeUserHaveToRest_ = timerToRest_;
      setRestTimer();
      if(!timeEndWarningShown_)
        {
          emit showTrayMessage("Less than 5min left");
        }

      timeEndWarningShown_ = true;
    }

  if (name.trimmed().isEmpty())
    {
      // some system windowless apps or no actual window is in focus
      return;
    }

  updateAppStats(name, time);

  LASTINPUTINFO info;
  info.cbSize = sizeof(info);
  GetLastInputInfo(&info);
  DWORD idleSeconds = (GetTickCount() - info.dwTime) / 1000;
  if (idleSeconds > USER_IDLING_DELAY)
    {
      daylyIdlingTime_++;
      activityTimer->pause();
    }
  else
    {
      activityTimer->start();
    }
}

void TimeTracker::updateDailyStats()
{
  appUsageModel->select();
  QSqlQuery insertQuery, updateQuery;
  insertQuery.prepare("INSERT OR IGNORE INTO DailyUsage(logon, logoff, idle, day, haveToRest,timeLeftToLock) VALUES(:logon, :logoff, :idle, date(:session), :haveToRest, :timeLeftToLock)");
  updateQuery.prepare("UPDATE DailyUsage SET timeLeftToLock = :timeLeftToLock, haveToRest = :haveToRest, logon = :logon, idle = :idle, logoff = :logoff WHERE day=date(:session)");

  insertQuery.bindValue(":timeLeftToLock", timeLeftToLock_.count());
  insertQuery.bindValue(":haveToRest", timeUserHaveToRest_.count());
  insertQuery.bindValue(":logon", daylyLoggedOnTime_.count());
  insertQuery.bindValue(":logoff", daylyLoggedOffTime_.count());
  insertQuery.bindValue(":idle", daylyIdlingTime_.count());
  insertQuery.bindValue(":session", currentSession_.toString("yyyy-MM-dd"));
  insertQuery.exec();

  updateQuery.bindValue(":timeLeftToLock", timeLeftToLock_.count());
  updateQuery.bindValue(":haveToRest", timeUserHaveToRest_.count());
  updateQuery.bindValue(":logon", daylyLoggedOnTime_.count());
  updateQuery.bindValue(":logoff", daylyLoggedOffTime_.count());
  updateQuery.bindValue(":idle", daylyIdlingTime_.count());
  updateQuery.bindValue(":session", currentSession_.toString("yyyy-MM-dd"));
  updateQuery.exec();

  appUsageModel->submitAll();
}

void TimeTracker::setRestTimer()
{  
  updateDailyStats();
}

