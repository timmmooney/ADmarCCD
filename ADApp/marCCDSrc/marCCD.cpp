/* marCCD.cpp
 *
 * This is a driver for a MAR CCD detector.
 *
 * Author: Mark Rivers
 *         University of Chicago
 *
 * Created:  June 11, 2008
 *
 */
 
#include <stddef.h>
#include <stdlib.h>
#include <stdarg.h>
#include <math.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <ctype.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <tiffio.h>

#include <epicsTime.h>
#include <epicsThread.h>
#include <epicsEvent.h>
#include <epicsTimer.h>
#include <epicsMutex.h>
#include <epicsString.h>
#include <epicsStdio.h>
#include <epicsMutex.h>
#include <cantProceed.h>

#include <asynOctetSyncIO.h>

#include "ADStdDriverParams.h"
#include "NDArray.h"
#include "ADDriver.h"

#include "drvMARCCD.h"

#define MAX_MESSAGE_SIZE 256 /* Messages to/from server */
#define MAX_FILENAME_LEN 256
#define MARCCD_DEFAULT_TIMEOUT 1.0 
/* Time between checking to see if TIFF file is complete */
#define FILE_READ_DELAY .01
#define MARCCD_POLL_DELAY .01
#define READ_TIFF_TIMEOUT 10.0

/* Task numbers */
#define TASK_ACQUIRE		0
#define TASK_READ		1
#define TASK_CORRECT		2
#define TASK_WRITE		3
#define TASK_DEZINGER		4

/* The status bits for each task are: */
/* Task Status bits */
#define TASK_STATUS_QUEUED	0x1
#define TASK_STATUS_EXECUTING	0x2
#define TASK_STATUS_ERROR	0x4
#define TASK_STATUS_RESERVED	0x8

/* This are the "old" states from version 0, but BUSY is also used in version 1 */
#define TASK_STATE_IDLE 0
#define TASK_STATE_ACQUIRE 1
#define TASK_STATE_READOUT 2
#define TASK_STATE_CORRECT 3
#define TASK_STATE_WRITING 4
#define TASK_STATE_ABORTING 5
#define TASK_STATE_UNAVAILABLE 6
#define TASK_STATE_ERROR 7
#define TASK_STATE_BUSY 8

/* These are the definitions of masks for looking at task state bits */
#define STATE_MASK		0xf
#define STATUS_MASK		0xf
#define TASK_STATUS_MASK(task)	(STATUS_MASK << (4*((task)+1)))

/* These are some convenient macros for checking and setting the state of each task */
/* They are used in the marccd code and can be used in the client code */
#define TASK_STATE(current_status) ((current_status) & STATE_MASK)
#define TASK_STATUS(current_status, task) (((current_status) & TASK_STATUS_MASK(task)) >> (4*((task) + 1)))
#define TEST_TASK_STATUS(current_status, task, status) (TASK_STATUS(current_status, task) & (status))

typedef enum {
    TMInternal,
    TMExternal,
    TMAlignment
} marCCDTriggerMode_t;

typedef enum {
    marCCDFrameNormal,
    marCCDFrameBackground,
    marCCDFrameRaw,
    marCCDFrameDoubleCorrelation
} marCCDFrameType_t;


static const char *driverName = "marCCD";

class marCCD : public ADDriver {
public:
    marCCD(const char *portName, const char *marCCDPort,
           int maxSizeX, int maxSizeY,
           int maxBuffers, size_t maxMemory);
                 
    /* These are the methods that we override from ADDriver */
    virtual asynStatus writeInt32(asynUser *pasynUser, epicsInt32 value);
    virtual asynStatus drvUserCreate(asynUser *pasynUser, const char *drvInfo, 
                                     const char **pptypeName, size_t *psize);
    void report(FILE *fp, int details);
                                        
    /* These are the methods that are new to this class */
    void marCCDTask();
    void abortAcquisition();
    asynStatus readTiff(const char *fileName, epicsTimeStamp *pStartTime, double timeout, NDArray *pImage);
    asynStatus writeServer(const char *output);
    asynStatus readServer(char *input, size_t maxChars, double timeout);
    asynStatus writeReadServer(const char *output, char *input, size_t maxChars, double timeout);
    int getState();
    void acquireFrame(double exposureTime, int useShutter);
    void readoutFrame(int bufferNumber, const char* fileName, int wait);
    void saveFile(int correctedFlag, int wait);
   
    /* Our data */
    epicsEventId startEventId;
    epicsEventId stopEventId;
    epicsTimerId timerId;
    char toServer[MAX_MESSAGE_SIZE];
    char fromServer[MAX_MESSAGE_SIZE];
    NDArray *pData;
    asynUser *pasynUserServer;
};

/* If we have any private driver parameters they begin with ADFirstDriverParam and should end
   with ADLastDriverParam, which is used for setting the size of the parameter library table */
typedef enum {
    marCCDTiffTimeout
        = ADFirstDriverParam,
    marCCDOverlap,
    marCCDTaskAcquireStatus,
    marCCDTaskReadoutStatus,
    marCCDTaskCorrectStatus,
    marCCDTaskWritingStatus,
    marCCDTaskDezingerStatus,
    ADLastDriverParam
} marCCDParam_t;

static asynParamString_t marCCDParamString[] = {
    {marCCDTiffTimeout,    "TIFF_TIMEOUT"},
    {marCCDOverlap,        "OVERLAP"},
    {marCCDTaskAcquireStatus,  "MAR_ACQUIRE_STATUS"},
    {marCCDTaskReadoutStatus,  "MAR_READOUT_STATUS"},
    {marCCDTaskCorrectStatus,  "MAR_CORRECT_STATUS"},
    {marCCDTaskWritingStatus,  "MAR_WRITING_STATUS"},
    {marCCDTaskDezingerStatus, "MAR_DEZINGER_STATUS"},
};

#define NUM_MARCCD_PARAMS (sizeof(marCCDParamString)/sizeof(marCCDParamString[0]))


/* This function reads TIFF files using libTiff.  It is not intended to be general,
 * it is intended to read the TIFF files that marCCDServer creates.  It checks to make sure
 * that the creation time of the file is after a start time passed to it, to force it to
 * wait for a new file to be created.
 */
 
asynStatus marCCD::readTiff(const char *fileName, epicsTimeStamp *pStartTime, double timeout, NDArray *pImage)
{
    int fd=-1;
    int fileExists=0;
    struct stat statBuff;
    epicsTimeStamp tStart, tCheck;
    time_t acqStartTime;
    double deltaTime;
    int status=-1;
    const char *functionName = "readTiff";
    int size, totalSize;
    int numStrips, strip;
    char *buffer;
    TIFF *tiff=NULL;
    epicsUInt32 uval;

    deltaTime = 0.;
    if (pStartTime) epicsTimeToTime_t(&acqStartTime, pStartTime);
    epicsTimeGetCurrent(&tStart);
    
    /* Suppress error messages from the TIFF library */
    TIFFSetErrorHandler(NULL);
    TIFFSetWarningHandler(NULL);
    
    while (deltaTime <= timeout) {
        fd = open(fileName, O_RDONLY, 0);
        if ((fd >= 0) && (timeout != 0.)) {
            fileExists = 1;
            /* The file exists.  Make sure it is a new file, not an old one.
             * We don't do this check if timeout==0, which is used for reading flat field files */
            status = fstat(fd, &statBuff);
            if (status){
                asynPrint(pasynUser, ASYN_TRACE_ERROR,
                    "%s::%s error calling fstat, errno=%d %s\n",
                    driverName, functionName, errno, fileName);
                close(fd);
                return(asynError);
            }
            /* We allow up to 10 second clock skew between time on machine running this IOC
             * and the machine with the file system returning modification time */
            if (difftime(statBuff.st_mtime, acqStartTime) > -10) break;
            close(fd);
            fd = -1;
        }
        /* Sleep, but check for stop event, which can be used to abort a long acquisition */
        status = epicsEventWaitWithTimeout(this->stopEventId, FILE_READ_DELAY);
        if (status == epicsEventWaitOK) {
            return(asynError);
        }
        epicsTimeGetCurrent(&tCheck);
        deltaTime = epicsTimeDiffInSeconds(&tCheck, &tStart);
    }
    if (fd < 0) {
        asynPrint(pasynUser, ASYN_TRACE_ERROR,
            "%s::%s timeout waiting for file to be created %s\n",
            driverName, functionName, fileName);
        if (fileExists) {
            asynPrint(pasynUser, ASYN_TRACE_ERROR,
                "  file exists but is more than 10 seconds old, possible clock synchronization problem\n");
        } 
        return(asynError);
    }
    close(fd);

    deltaTime = 0.;
    while (deltaTime <= timeout) {
        /* At this point we know the file exists, but it may not be completely written yet.
         * If we get errors then try again */
        tiff = TIFFOpen(fileName, "rc");
        if (tiff == NULL) {
            status = asynError;
            goto retry;
        }
        
        /* Do some basic checking that the image size is what we expect */
        status = TIFFGetField(tiff, TIFFTAG_IMAGEWIDTH, &uval);
        if (uval != (epicsUInt32)pImage->dims[0].size) {
            asynPrint(pasynUser, ASYN_TRACE_ERROR,
                "%s::%s, image width incorrect =%u, should be %d\n",
                driverName, functionName, uval, pImage->dims[0].size);
            goto retry;
        }
        status = TIFFGetField(tiff, TIFFTAG_IMAGELENGTH, &uval);
        if (uval != (epicsUInt32)pImage->dims[1].size) {
            asynPrint(pasynUser, ASYN_TRACE_ERROR,
                "%s::%s, image length incorrect =%u, should be %d\n",
                driverName, functionName, uval, pImage->dims[1].size);
            goto retry;
        }
        numStrips= TIFFNumberOfStrips(tiff);
        buffer = (char *)pImage->pData;
        totalSize = 0;
        for (strip=0; (strip < numStrips) && (totalSize < pImage->dataSize); strip++) {
            size = TIFFReadEncodedStrip(tiff, 0, buffer, pImage->dataSize-totalSize);
            if (size == -1) {
                /* There was an error reading the file.  Most commonly this is because the file
                 * was not yet completely written.  Try again. */
                asynPrint(pasynUser, ASYN_TRACE_FLOW,
                    "%s::%s, error reading TIFF file %s\n",
                    driverName, functionName, fileName);
                goto retry;
            }
            buffer += size;
            totalSize += size;
        }
        if (totalSize != pImage->dataSize) {
            status = asynError;
            asynPrint(pasynUser, ASYN_TRACE_ERROR,
                "%s::%s, file size incorrect =%d, should be %d\n",
                driverName, functionName, totalSize, pImage->dataSize);
            goto retry;
        }
        /* Sucesss! */
        break;
        
        retry:
        if (tiff != NULL) TIFFClose(tiff);
        tiff = NULL;
        /* Sleep, but check for stop event, which can be used to abort a long acquisition */
        status = epicsEventWaitWithTimeout(this->stopEventId, FILE_READ_DELAY);
        if (status == epicsEventWaitOK) {
            return(asynError);
        }
        epicsTimeGetCurrent(&tCheck);
        deltaTime = epicsTimeDiffInSeconds(&tCheck, &tStart);
    }

    if (tiff != NULL) TIFFClose(tiff);

    return(asynSuccess);
}   

asynStatus marCCD::writeServer(const char *output)
{
    size_t nwrite;
    asynStatus status;
    asynUser *pasynUser = this->pasynUserServer;
    const char *functionName="writeServer";

    /* Flush any stale input, since the next operation is likely to be a read */
    status = pasynOctetSyncIO->flush(pasynUser);
    status = pasynOctetSyncIO->write(pasynUser, output,
                                     strlen(output), MARCCD_DEFAULT_TIMEOUT,
                                     &nwrite);
                                        
    if (status) asynPrint(pasynUser, ASYN_TRACE_ERROR,
                    "%s:%s, status=%d, sent\n%s\n",
                    driverName, functionName, status, output);

    /* Set output string so it can get back to EPICS */
    setStringParam(ADStringToServer, output);
    
    return(status);
}


asynStatus marCCD::readServer(char *input, size_t maxChars, double timeout)
{
    size_t nread;
    asynStatus status=asynSuccess;
    asynUser *pasynUser = this->pasynUserServer;
    int eomReason;
    const char *functionName="readServer";

    status = pasynOctetSyncIO->read(pasynUser, input, maxChars, timeout,
                                    &nread, &eomReason);
    if (status) asynPrint(pasynUser, ASYN_TRACE_ERROR,
                    "%s:%s, timeout=%f, status=%d received %d bytes\n%s\n",
                    driverName, functionName, timeout, status, nread, input);
    /* Set output string so it can get back to EPICS */
    setStringParam(ADStringFromServer, input);

    return(status);
}

asynStatus marCCD::writeReadServer(const char *output, char *input, size_t maxChars, double timeout)
{
    asynStatus status;
    
    status = writeServer(output);
    if (status) return status;
    status = readServer(input, maxChars, timeout);
    return status;
}

int marCCD::getState()
{
    int marStatus;
    ADStatus_t adStatus = ADStatusError;
    asynStatus status;
    int acquireStatus, readoutStatus, correctStatus, writingStatus, dezingerStatus;
    
    status = writeReadServer("get_state", this->fromServer, sizeof(this->fromServer),
                              MARCCD_DEFAULT_TIMEOUT);
    if (status) return(adStatus);
    marStatus = strtol(this->fromServer, NULL, 0);
    acquireStatus = TASK_STATUS(marStatus, TASK_ACQUIRE); 
    readoutStatus = TASK_STATUS(marStatus, TASK_READ); 
    correctStatus = TASK_STATUS(marStatus, TASK_CORRECT); 
    writingStatus = TASK_STATUS(marStatus, TASK_WRITE); 
    dezingerStatus = TASK_STATUS(marStatus, TASK_DEZINGER);
    setIntegerParam(marCCDTaskAcquireStatus, acquireStatus);
    setIntegerParam(marCCDTaskReadoutStatus, readoutStatus);
    setIntegerParam(marCCDTaskCorrectStatus, correctStatus);
    setIntegerParam(marCCDTaskWritingStatus, writingStatus);
    setIntegerParam(marCCDTaskDezingerStatus, dezingerStatus);
    if (marStatus == 0) adStatus = ADStatusIdle;
    else if (acquireStatus & (TASK_STATUS_QUEUED | TASK_STATUS_EXECUTING)) adStatus = ADStatusAcquire;
    else if (readoutStatus & (TASK_STATUS_QUEUED | TASK_STATUS_EXECUTING)) adStatus = ADStatusReadout;
    else if (correctStatus & (TASK_STATUS_QUEUED | TASK_STATUS_EXECUTING)) adStatus = ADStatusCorrect;
    else if (writingStatus & (TASK_STATUS_QUEUED | TASK_STATUS_EXECUTING)) adStatus = ADStatusSaving;
    if ((acquireStatus | readoutStatus | correctStatus | writingStatus | dezingerStatus) & 
        TASK_STATUS_ERROR) adStatus = ADStatusError;
    setIntegerParam(ADStatus, adStatus);
    callParamCallbacks();
    return(marStatus);
}

/* This function is called when the exposure time timer expires */
static void timerCallbackC(void *drvPvt)
{
    marCCD *pPvt = (marCCD *)drvPvt;
    
   epicsEventSignal(pPvt->stopEventId);
}


void marCCD::acquireFrame(double exposureTime, int useShutter)
{
    double delay;
    int status;
    epicsTimeStamp startTime, currentTime;
    double timeRemaining;
    double shutterOpenDelay, shutterCloseDelay;

    /* Wait for the acquire task to be done with the previous acquisition, if any */    
    status = getState();
    while (TEST_TASK_STATUS(status, TASK_ACQUIRE, TASK_STATUS_EXECUTING) || 
           TASK_STATE(status) >= 8) {
        epicsThreadSleep(MARCCD_POLL_DELAY);
        status = getState();
    }

    setStringParam(ADStatusMessage, "Starting exposure");
    setIntegerParam(ADStatus, ADStatusAcquire);
    writeServer("start");
    callParamCallbacks();
   
    /* Wait for acquisition to actually start */
    status = getState();
    while (!TEST_TASK_STATUS(status, TASK_ACQUIRE, TASK_STATUS_EXECUTING) || 
           TASK_STATE(status) >= 8) {
        epicsThreadSleep(MARCCD_POLL_DELAY);
        status = getState();
    }
    
    /* Set the the start time for the TimeRemaining counter */
    epicsTimeGetCurrent(&startTime);
    timeRemaining = exposureTime;
    if (useShutter) {
        /* Open the shutter */
        writeServer("shutter,1");
        /* This delay is to get the exposure time correct.  
         * It is equal to the opening time of the shutter minus the
         * closing time.  If they are equal then no delay is needed, 
         * except use 1msec so delay is not negative and commands are 
         * not back-to-back */
        getDoubleParam(ADShutterOpenDelay, &shutterOpenDelay);
        getDoubleParam(ADShutterCloseDelay, &shutterCloseDelay);
        delay = shutterOpenDelay - shutterCloseDelay;
        if (delay < .001) delay=.001;
        epicsThreadSleep(delay);
    }

    /* Wait for the exposure time using epicsEventWaitWithTimeout, 
     * so we can abort */
    epicsTimerStartDelay(this->timerId, exposureTime);
    while(1) {
        status = epicsEventWaitWithTimeout(this->stopEventId, MARCCD_POLL_DELAY);
        if (status == epicsEventWaitOK) break;
        epicsTimeGetCurrent(&currentTime);
        timeRemaining = exposureTime - 
            epicsTimeDiffInSeconds(&currentTime, &startTime);
        if (timeRemaining < 0.) timeRemaining = 0.;
        setDoubleParam(ADTimeRemaining, timeRemaining);
        callParamCallbacks();
    }
    if (useShutter) {
        /* Close shutter */
        writeServer("shutter,0");
        epicsThreadSleep(shutterCloseDelay);
    }
}

void marCCD::readoutFrame(int bufferNumber, const char* fileName, int wait)
{
    int status;
    
     /* Wait for the readout task to be done with the previous frame, if any */    
    status = getState();
    while (TEST_TASK_STATUS(status, TASK_READ, TASK_STATUS_EXECUTING | TASK_STATUS_QUEUED) || 
           TASK_STATE(status) >= 8) {
        epicsThreadSleep(MARCCD_POLL_DELAY);
        status = getState();
    }

    setIntegerParam(ADStatus, ADStatusReadout);
    callParamCallbacks();
    if (fileName && strlen(fileName)!=0) {
        epicsSnprintf(this->toServer, sizeof(this->toServer), "readout,%d,%s", bufferNumber, fileName);
    } else {
        epicsSnprintf(this->toServer, sizeof(this->toServer), "readout,%d", bufferNumber);
    }
    writeServer(this->toServer);

    /* Wait for the readout to complete */
    status = getState();
    while (TEST_TASK_STATUS(status, TASK_READ, TASK_STATUS_EXECUTING | TASK_STATUS_QUEUED)) {
        epicsThreadSleep(MARCCD_POLL_DELAY);
        status = getState();
    }
    
    if (!wait) return;
    
    /* If the filename was specified wait for the write to complete */
    if (!fileName || strlen(fileName)==0) return;
    status = getState();
    while (TEST_TASK_STATUS(status, TASK_WRITE, TASK_STATUS_EXECUTING | TASK_STATUS_QUEUED) || 
           TASK_STATE(status) >= 8) {
        epicsThreadSleep(MARCCD_POLL_DELAY);
        status = getState();
    }
}
 
void marCCD::saveFile(int correctedFlag, int wait)
{
    char fullFileName[MAX_FILENAME_LEN];
    int status;

    /* Wait for any previous write to complete */
    status = getState();
    while (TEST_TASK_STATUS(status, TASK_WRITE, TASK_STATUS_EXECUTING | TASK_STATUS_QUEUED) || 
           TASK_STATE(status) >= 8) {
        epicsThreadSleep(MARCCD_POLL_DELAY);
        status = getState();
    }
    createFileName(MAX_FILENAME_LEN, fullFileName);
    epicsSnprintf(this->toServer, sizeof(this->toServer), "writefile,%s,%d", 
                  fullFileName, correctedFlag);
    writeServer(this->toServer);
    if (!wait) return;
    status = getState();
    while (TEST_TASK_STATUS(status, TASK_WRITE, TASK_STATUS_EXECUTING | TASK_STATUS_QUEUED) || 
           TASK_STATE(status) >= 8) {
        epicsThreadSleep(MARCCD_POLL_DELAY);
        status = getState();
    }
}

static void marCCDTaskC(void *drvPvt)
{
    marCCD *pPvt = (marCCD *)drvPvt;
    
    pPvt->marCCDTask();
}

void marCCD::marCCDTask()
{
    /* This thread controls acquisition, reads TIFF files to get the image data, and
     * does the callbacks to send it to higher layers */
    int status = asynSuccess;
    int imageCounter;
    int numImages;
    int acquire;
    ADStatus_t acquiring;
    NDArray *pImage;
    double acquireTime, acquirePeriod;
    int triggerMode;
    int frameType;
    int autoSave;
    int overlap, wait;
    int bufferNumber;
    int shutterMode, useShutter;
    epicsTimeStamp startTime;
    const char *functionName = "marCCDTask";
    char fullFileName[MAX_FILENAME_LEN];
    char filePath[MAX_FILENAME_LEN];
    char statusMessage[MAX_MESSAGE_SIZE];
    int dims[2];

    epicsMutexLock(this->mutexId);

    /* Loop forever */
    while (1) {
        /* Is acquisition active? */
        getIntegerParam(ADAcquire, &acquire);
        
        /* If we are not acquiring then wait for a semaphore that is given when acquisition is started */
        if (!acquire) {
            setStringParam(ADStatusMessage, "Waiting for acquire command");
            setIntegerParam(ADStatus, ADStatusIdle);
            callParamCallbacks();
            /* Release the lock while we wait for an event that says acquire has started, then lock again */
            epicsMutexUnlock(this->mutexId);
            asynPrint(this->pasynUser, ASYN_TRACE_FLOW, 
                "%s:%s: waiting for acquire to start\n", driverName, functionName);
            status = epicsEventWait(this->startEventId);
            epicsMutexLock(this->mutexId);
            getIntegerParam(ADAcquire, &acquire);
        }
        
        /* What to do here depends on the frame type */
        getIntegerParam(ADFrameType, &frameType);
        getDoubleParam(ADAcquireTime, &acquireTime);
        getIntegerParam(ADAutoSave, &autoSave);
        getIntegerParam(marCCDOverlap, &overlap);
        getIntegerParam(ADShutterMode, &shutterMode);
        if (overlap) wait=0; else wait=1;
        if (shutterMode == ADShutterModeDetector) useShutter=1; else useShutter=0;
        
        strcpy(fullFileName, "");
        if (autoSave) createFileName(MAX_FILENAME_LEN, fullFileName);
        
        switch(frameType) {
            case marCCDFrameNormal:
            case marCCDFrameRaw:
                acquireFrame(acquireTime, useShutter);
                if (frameType == marCCDFrameNormal) bufferNumber=0; else bufferNumber=3;
                readoutFrame(bufferNumber, fullFileName, wait);
                break;
            case marCCDFrameBackground:
                acquireFrame(.001, 0);
                readoutFrame(1, NULL, 1);
                acquireFrame(.001, 0);
                readoutFrame(2, NULL, 1);
                writeServer("dezinger,1");
                status = getState();
                while (TEST_TASK_STATUS(status, TASK_DEZINGER, 
                                        TASK_STATUS_EXECUTING | TASK_STATUS_QUEUED) || 
                                        TASK_STATE(status) >= 8) {
                    epicsThreadSleep(MARCCD_POLL_DELAY);
                    status = getState();
                }
                break;
            case marCCDFrameDoubleCorrelation:
                acquireFrame(acquireTime/2., useShutter);
                readoutFrame(2, NULL, 1);
                acquireFrame(acquireTime/2., useShutter);
                readoutFrame(0, NULL, 1);
                writeServer("dezinger,0");
                status = getState();
                while (TEST_TASK_STATUS(status, TASK_DEZINGER, 
                                        TASK_STATUS_EXECUTING | TASK_STATUS_QUEUED) || 
                                        TASK_STATE(status) >= 8) {
                    epicsThreadSleep(MARCCD_POLL_DELAY);
                    status = getState();
                }
                if (autoSave) saveFile(1, 1);
        }
        
        /* Inquire about the image dimensions */
        writeReadServer("get_size", this->fromServer, sizeof(this->fromServer), MARCCD_DEFAULT_TIMEOUT);
        sscanf(this->fromServer, "%d,%d", &dims[0], &dims[1]);
        setIntegerParam(ADImageSizeX, dims[0]);
        setIntegerParam(ADImageSizeY, dims[1]);
        pImage = this->pNDArrayPool->alloc(2, dims, NDUInt16, 0, NULL);

        epicsSnprintf(statusMessage, sizeof(statusMessage), "Reading TIFF file %s", fullFileName);
        setStringParam(ADStatusMessage, statusMessage);
        callParamCallbacks();
        status = readTiff(fullFileName, &startTime, READ_TIFF_TIMEOUT, pImage); 

        getIntegerParam(ADImageCounter, &imageCounter);
        imageCounter++;
        setIntegerParam(ADImageCounter, imageCounter);
        /* Call the callbacks to update any changes */
        callParamCallbacks();

        /* Put the frame number and time stamp into the buffer */
        pImage->uniqueId = imageCounter;
        pImage->timeStamp = startTime.secPastEpoch + startTime.nsec / 1.e9;

        /* Call the NDArray callback */
        /* Must release the lock here, or we can get into a deadlock, because we can
         * block on the plugin lock, and the plugin can be calling us */
        epicsMutexUnlock(this->mutexId);
        asynPrint(this->pasynUser, ASYN_TRACE_FLOW, 
             "%s:%s: calling NDArray callback\n", driverName, functionName);
        doCallbacksGenericPointer(pImage, NDArrayData, 0);
        epicsMutexLock(this->mutexId);

        /* Free the image buffer */
        pImage->release();
        setIntegerParam(ADAcquire, 0);

        /* Call the callbacks to update any changes */
        callParamCallbacks();
    }
}


asynStatus marCCD::writeInt32(asynUser *pasynUser, epicsInt32 value)
{
    int function = pasynUser->reason;
    int adstatus, binX, binY;
    int addr=0;
    int correctedFlag, frameType;
    asynStatus status = asynSuccess;
    const char *functionName = "writeInt32";

    status = setIntegerParam(addr, function, value);

    switch (function) {
    case ADAcquire:
        getIntegerParam(addr, ADStatus, &adstatus);
        if (value && (adstatus == ADStatusIdle)) {
            /* Send an event to wake up the marCCD task.  */
            epicsEventSignal(this->startEventId);
        } 
        if (!value) {
            /* This was a command to stop acquisition */
            epicsEventSignal(this->stopEventId);
            writeServer("abort");
        }
        break;
    case ADBinX:
    case ADBinY:
        /* Set binning */
        getIntegerParam(addr, ADBinX, &binX);
        getIntegerParam(addr, ADBinY, &binY);
        epicsSnprintf(this->toServer, sizeof(this->toServer), "set_bin,%d,%d", binX, binY);
        writeServer(this->toServer);
        break;
    
    case ADWriteFile:
        getIntegerParam(addr, ADFrameType, &frameType);
        if (frameType == marCCDFrameRaw) correctedFlag=0; else correctedFlag=1;
        saveFile(correctedFlag, 1);
    }
        
    /* Do callbacks so higher layers see any changes */
    callParamCallbacks(addr, addr);
    
    if (status) 
        asynPrint(pasynUser, ASYN_TRACE_ERROR, 
              "%s:%s: error, status=%d function=%d, value=%d\n", 
              driverName, functionName, status, function, value);
    else        
        asynPrint(pasynUser, ASYN_TRACEIO_DRIVER, 
              "%s:%s: function=%d, value=%d\n", 
              driverName, functionName, function, value);
    return status;
}



/* asynDrvUser routines */
asynStatus marCCD::drvUserCreate(asynUser *pasynUser,
                                      const char *drvInfo, 
                                      const char **pptypeName, size_t *psize)
{
    asynStatus status;
    int param;
    const char *functionName = "drvUserCreate";

    /* See if this is one of our standard parameters */
    status = findParam(marCCDParamString, NUM_MARCCD_PARAMS, 
                       drvInfo, &param);
                                
    if (status == asynSuccess) {
        pasynUser->reason = param;
        if (pptypeName) {
            *pptypeName = epicsStrDup(drvInfo);
        }
        if (psize) {
            *psize = sizeof(param);
        }
        asynPrint(pasynUser, ASYN_TRACE_FLOW,
                  "%s:%s: drvInfo=%s, param=%d\n", 
                  driverName, functionName, drvInfo, param);
        return(asynSuccess);
    }
    
    /* If not, then see if it is a base class parameter */
    status = ADDriver::drvUserCreate(pasynUser, drvInfo, pptypeName, psize);
    return(status);  
}
    
void marCCD::report(FILE *fp, int details)
{
    int addr=0;

    fprintf(fp, "MAR-CCD detector %s\n", this->portName);
    if (details > 0) {
        int nx, ny, dataType;
        getIntegerParam(addr, ADSizeX, &nx);
        getIntegerParam(addr, ADSizeY, &ny);
        getIntegerParam(addr, ADDataType, &dataType);
        fprintf(fp, "  NX, NY:            %d  %d\n", nx, ny);
        fprintf(fp, "  Data type:         %d\n", dataType);
    }
    /* Invoke the base class method */
    ADDriver::report(fp, details);
}

extern "C" int marCCDConfig(const char *portName, const char *serverPort, 
                            int maxSizeX, int maxSizeY,
                            int maxBuffers, size_t maxMemory)
{
    new marCCD(portName, serverPort, maxSizeX, maxSizeY, maxBuffers, maxMemory);
    return(asynSuccess);
}

marCCD::marCCD(const char *portName, const char *serverPort,
                                int maxSizeX, int maxSizeY,
                                int maxBuffers, size_t maxMemory)

    : ADDriver(portName, 1, ADLastDriverParam, maxBuffers, maxMemory, 0, 0), 
      pData(NULL)

{
    int status = asynSuccess;
    epicsTimerQueueId timerQ;
    const char *functionName = "marCCD";
    int addr=0;
    int dims[2];

    /* Create the epicsEvents for signaling to the marCCD task when acquisition starts and stops */
    this->startEventId = epicsEventCreate(epicsEventEmpty);
    if (!this->startEventId) {
        printf("%s:%s epicsEventCreate failure for start event\n", 
            driverName, functionName);
        return;
    }
    this->stopEventId = epicsEventCreate(epicsEventEmpty);
    if (!this->stopEventId) {
        printf("%s:%s epicsEventCreate failure for stop event\n", 
            driverName, functionName);
        return;
    }
    
    /* Create the epicsTimerQueue for exposure time handling */
    timerQ = epicsTimerQueueAllocate(1, epicsThreadPriorityScanHigh);
    this->timerId = epicsTimerQueueCreateTimer(timerQ, timerCallbackC, this);
    
    /* Allocate the raw buffer we use to readTiff files.  Only do this once */
    dims[0] = maxSizeX;
    dims[1] = maxSizeY;
    this->pData = this->pNDArrayPool->alloc(2, dims, NDUInt32, 0, NULL);
    
    /* Connect to server */
    status = pasynOctetSyncIO->connect(serverPort, 0, &this->pasynUserServer, NULL);
    
    /* Read the current state of the server */
    status = getState();

    /* Set some default values for parameters */
    status =  setStringParam (addr, ADManufacturer, "MAR");
    status |= setStringParam (addr, ADModel, "CCD");
    status |= setIntegerParam(addr, ADMaxSizeX, maxSizeX);
    status |= setIntegerParam(addr, ADMaxSizeY, maxSizeY);
    status |= setIntegerParam(addr, ADSizeX, maxSizeX);
    status |= setIntegerParam(addr, ADSizeX, maxSizeX);
    status |= setIntegerParam(addr, ADSizeY, maxSizeY);
    status |= setIntegerParam(addr, ADImageSizeX, maxSizeX);
    status |= setIntegerParam(addr, ADImageSizeY, maxSizeY);
    status |= setIntegerParam(addr, ADImageSize, 0);
    status |= setIntegerParam(addr, ADDataType,  NDUInt32);
    status |= setIntegerParam(addr, ADImageMode, ADImageContinuous);
    status |= setIntegerParam(addr, ADTriggerMode, TMInternal);
    status |= setDoubleParam (addr, ADAcquireTime, 1.);
    status |= setDoubleParam (addr, ADAcquirePeriod, 0.);
    status |= setIntegerParam(addr, ADNumImages, 1);
    status |= setIntegerParam(addr, marCCDOverlap, 0);

    status |= setDoubleParam (addr, marCCDTiffTimeout, 20.);
       
    if (status) {
        printf("%s: unable to set camera parameters\n", functionName);
        return;
    }
    
    /* Create the thread that updates the images */
    status = (epicsThreadCreate("marCCDTask",
                                epicsThreadPriorityMedium,
                                epicsThreadGetStackSize(epicsThreadStackMedium),
                                (EPICSTHREADFUNC)marCCDTaskC,
                                this) == NULL);
    if (status) {
        printf("%s:%s epicsThreadCreate failure for image task\n", 
            driverName, functionName);
        return;
    }
}
