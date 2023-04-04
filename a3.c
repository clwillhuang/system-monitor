#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/wait.h>

#include "printUsers.h"
#include "printSystem.h"
#include "stringUtils.h"
#include "parseArguments.h"
#include "parseCpuStats.h"
#include "parseMemoryStats.h"

#define FD_READ 0
#define FD_WRITE 1

#define MEM_FDS 0 
#define USER_FDS 1
#define CPU_FDS 2

int main(int argc, char **argv)
{
    /**
     * Show only the system usage? (--system)
     */
    bool showSystem = false;
    /**
     * Show only the user's usage? (--user)
     */
    bool showUser = false;
    /**
     * Show graphical output for memory and CPU utilization? (--graphics)
     */
    bool showGraphics = false;
    /**
     * Output information sequentially without refreshing screen? (--sequential)
     */
    bool showSequential = false;
    /**
     * The number of times that the usage statistics will be sampled. (--samples). Default = 10;
     */
    long numSamples = 10;
    /**
     *  The time between consecutive samples of the usage statistics, in seconds (--tdelay). Default = 1
     */
    long sampleDelay = 1;

    /**
     * Fds of pipes used to read data from children.
    */
    int readFromChildFds[3][2];

    /**
     * Pipe for notifying parent of incoming data.
    */
    int incomingDataPipe[2];

    /**
     * Fds of pipes used to write data to children
    */
    int writeToChildFds[3][2];

    // parse command line arguments
    if (parseArguments(argc, argv, &showSystem, &showUser, &showGraphics, &showSequential, &numSamples, &sampleDelay) != 0)
    {
        return 1;
    }
    printf("\033[2J\033[3J");

    printf("\033[2J\033[H\n");

    // printf("Parsed arguments: --system %d --user %d --graphics %d --sequential %d numSamples %ld samplesDelay %ld\n",
    //        showSystem, showUser, showGraphics, showSequential, numSamples, sampleDelay);

    // store past computed cpu utilization for each sample + initial data point
    struct cpuDataSample cpuData[numSamples + 1];
    float cpuHistory[numSamples];
    for (int i = 0; i < numSamples; i++)
    {
        cpuHistory[i] = 0.0;
    }

    // store previously calculated memory data
    struct memorySample memorySamples[numSamples];
    // store previously calculated output
    char* memoryOutput[numSamples];
    char* cpuOutput[numSamples];
    char* userInfo;
    for (int i = 0;  i < numSamples; i++) {
        memoryOutput[i] = NULL;
        cpuOutput[i] = NULL;
    }
    
    int processorCount, coreCount;
    char* averageCpuUsage = NULL;

    // record initial CPU stats
    if (recordCpuStats(&cpuData[0]) != 0)
    {
        return 1;
    }
    sleep(sampleDelay);

    pipe(incomingDataPipe);

    // create child processes
    if (showSystem || !showUser) { // Show memory utilization
        pipe(writeToChildFds[MEM_FDS]); // create pipe for parent -> child
        pipe(readFromChildFds[MEM_FDS]); // pipe for child -> parent
        pid_t memoryPid = fork(); // memory usage process
        if (memoryPid == 0) { // child process
            close(writeToChildFds[MEM_FDS][FD_WRITE]);
            close(readFromChildFds[MEM_FDS][FD_READ]); // prevent child from reading data meant for parent
            close(incomingDataPipe[FD_READ]);
            displayMemory(showGraphics, writeToChildFds[MEM_FDS], readFromChildFds[MEM_FDS], incomingDataPipe);
            exit(0);
        } else {
            close(writeToChildFds[MEM_FDS][FD_READ]); 
            close(readFromChildFds[MEM_FDS][FD_WRITE]); 
        }

        pipe(writeToChildFds[CPU_FDS]); // create pipe for parent -> child
        pipe(readFromChildFds[CPU_FDS]); // pipe for child -> parent
        pid_t cpuPid = fork();
        if (cpuPid == 0) {
            close(writeToChildFds[CPU_FDS][FD_WRITE]);
            close(readFromChildFds[CPU_FDS][FD_READ]); 
            close(incomingDataPipe[FD_READ]);
            displayCpu(showGraphics, writeToChildFds[CPU_FDS], readFromChildFds[CPU_FDS], incomingDataPipe);
            exit(0);
        } else {
            close(writeToChildFds[CPU_FDS][FD_READ]); 
            close(readFromChildFds[CPU_FDS][FD_WRITE]); 
        }

        pipe(writeToChildFds[USER_FDS]); // create pipe for parent -> child
        pipe(readFromChildFds[USER_FDS]); // pipe for child -> parent
        pid_t userPid = fork();
        if (userPid == 0) {
            close(writeToChildFds[USER_FDS][FD_WRITE]);
            close(readFromChildFds[USER_FDS][FD_READ]); 
            close(incomingDataPipe[FD_READ]);
            printUsers(writeToChildFds[USER_FDS], readFromChildFds[USER_FDS], incomingDataPipe);
            exit(0);
        } else {
            close(writeToChildFds[USER_FDS][FD_READ]); 
            close(readFromChildFds[USER_FDS][FD_WRITE]); 
        }
    }

    close(incomingDataPipe[FD_WRITE]);

    for (int thisSample = 0; thisSample < numSamples; thisSample++)
    {
        // ensure this iteration's info is empty
        memoryOutput[thisSample] = NULL;
        cpuOutput[thisSample] = NULL;
        if (userInfo != NULL) {
            free(userInfo);
            userInfo = NULL;
        }
        if (averageCpuUsage != NULL) {
            free(averageCpuUsage); 
            averageCpuUsage = NULL;
        }
        
        // PASS DATA TO PROCESSES
        
        if (showSystem || !showUser) { 
            
            // MEMORY USAGE (system information)
            int temp = MEM_START_FLAG;
            write(writeToChildFds[MEM_FDS][FD_WRITE], &temp, sizeof(int));
            write(writeToChildFds[MEM_FDS][FD_WRITE], &thisSample, sizeof(int));
            if (thisSample > 0) {
                write(writeToChildFds[MEM_FDS][FD_WRITE], memorySamples + thisSample - 1, sizeof(struct memorySample));
            } 

            // CPU USAGE
            temp = CPU_START_FLAG;
            write(writeToChildFds[CPU_FDS][FD_WRITE], &temp, sizeof(int));
            write(writeToChildFds[CPU_FDS][FD_WRITE], &thisSample, sizeof(int));
            if (thisSample > 0) {
                write(writeToChildFds[CPU_FDS][FD_WRITE], cpuHistory + thisSample, sizeof(float)); // write CPU % of previous iteration
                write(writeToChildFds[CPU_FDS][FD_WRITE], cpuData + thisSample - 1, sizeof(struct cpuDataSample));
            }

            // USERS
            temp = USER_START_FLAG;
            write(writeToChildFds[USER_FDS][FD_WRITE], &temp, sizeof(int));
        }

        printf("Passed data\n");

        // read output from children
        while (memoryOutput[thisSample] == NULL || cpuOutput[thisSample] == NULL || userInfo == NULL) {
            int processFunction = 0, strLen = 0;
            read(incomingDataPipe[FD_READ], &processFunction, sizeof(int)); 
            printf("Received info of type %d", processFunction);
            switch (processFunction)
            {
                case MEM_DATA_ID:
                    read(readFromChildFds[MEM_FDS][FD_READ], memorySamples + thisSample, sizeof(MemorySample));
                    read(readFromChildFds[MEM_FDS][FD_READ], &strLen, sizeof(int));
                    memoryOutput[thisSample] = (char*)malloc(sizeof(char) * (strLen + 1)); 
                    read(readFromChildFds[MEM_FDS][FD_READ], memoryOutput[thisSample], sizeof(char) * (strLen + 1));
                    break;

                case CPU_DATA_ID:
                    read(readFromChildFds[CPU_FDS][FD_READ], &processorCount, sizeof(int));
                    read(readFromChildFds[CPU_FDS][FD_READ], &coreCount, sizeof(int));

                    read(readFromChildFds[CPU_FDS][FD_READ], cpuData + thisSample, sizeof(CpuDataSample));

                    // read average CPU usage line
                    read(readFromChildFds[CPU_FDS][FD_READ], &strLen, sizeof(int));
                    averageCpuUsage = (char*)malloc(sizeof(char) * (strLen + 1)); 
                    read(readFromChildFds[CPU_FDS][FD_READ], averageCpuUsage, sizeof(char) * (strLen + 1));
                    
                    // read timepoint CPU utilization
                    read(readFromChildFds[CPU_FDS][FD_READ], &strLen, sizeof(int));
                    cpuOutput[thisSample] = (char*)malloc(sizeof(char) * (strLen + 1)); 
                    read(readFromChildFds[CPU_FDS][FD_READ], cpuOutput[thisSample], sizeof(char) * (strLen + 1));
                    break;

                case USER_DATA_ID:
                    read(readFromChildFds[USER_FDS][FD_READ], &strLen, sizeof(int));
                    userInfo = (char*)malloc(sizeof(char) * (strLen + 1)); 
                    read(readFromChildFds[USER_FDS][FD_READ], userInfo, sizeof(char) * (strLen + 1));
                
                default:
                    break;
            }
        }

        printf("Read data\n");

        if (!showSequential)
        {
            // use escape characters to make it appear that screen is refreshing
            // \033[3J erases saved lines
            // \033[H repositions cursor to start
            // \033[2J erases entire screen
            printf("\033[2J\033[3J\033[2J\033[H\n");
        }

        printf("\n||| Sample #%d |||\n", thisSample + 1);
        printDivider();
        printf("Nbr of samples: %ld -- every %ld secs\n", numSamples, sampleDelay);

        struct rusage rUsageData;
        getrusage(RUSAGE_SELF, &rUsageData);
        printf("Memory usage: %ld kilobytes\n", rUsageData.ru_maxrss);

        printDivider();

        if (showGraphics)
        {
            printf("### Memory ### (Phys.Used/Tot -- Virtual Used/Tot, Memory Graphic)\n");
        }
        else
        {
            printf("### Memory ### (Phys.Used/Tot -- Virtual Used/Tot)\n");
        }

        for (int i = 0; i < numSamples; i++) {
            if (memoryOutput[i] == NULL) 
                printf("\n");
            else
                printf("%s", memoryOutput[i]);
        }

        printDivider();
    
        // USER CONNECTIONS (user information)
        if (showUser || !showSystem)
        {
            printf("### Sessions/users ###\n");
            printf("%s", userInfo);
            printDivider();
        }

        printf("Number of processors: %d\n", processorCount);
        printf("Total number of cores: %d\n", coreCount);
        // Print the average CPU utilization from beginning to current sample
        printf("%s", averageCpuUsage);

        printDivider();

        if (showGraphics)
        {
            printf("CPU Utilization (%% Use, Relative Abs. Change, %% Use Graphic)\n");
        }
        else
        {
            printf("CPU Utilization (%% Use, Relative Abs. Change)\n");
        }

        for (int i = 0; i < numSamples; i++) {
            if (cpuOutput[i] == NULL) 
                printf("\n");
            else
                printf("%s", cpuOutput[i]);
        }

        printDivider();

        printf("||| End of Sample #%d |||\n\n\n", thisSample + 1);

        if (thisSample < numSamples - 1)
        {
            sleep(sampleDelay);
        }
    }

    printDivider();
    if (printSystemInfo() != 0) {
        return 1;
    }
    printDivider();

    if (averageCpuUsage != NULL) {
        free(averageCpuUsage);
    }
    for (int thisSample = 0; thisSample < numSamples; thisSample++)
    {
        if (memoryOutput[thisSample] != NULL)
        free(memoryOutput[thisSample]);
        if (cpuOutput[thisSample] != NULL)
            free(cpuOutput[thisSample]);
    }

    // tell children to exit
    int temp = -1;
    for (int i = 0; i < 3; i++) {
        write(writeToChildFds[i][FD_WRITE], &temp, sizeof(int));
    }

    for (int i = 0; i < 3; i++) {
        int status;
        pid_t w = wait(&status);
        printf("Child %d exited\n", w);
    }

    for (int i = 0; i < 3; i++) {
        close(writeToChildFds[i][FD_READ]);
        close(writeToChildFds[i][FD_WRITE]);
        close(readFromChildFds[i][FD_READ]);
        close(readFromChildFds[i][FD_WRITE]);
    }
    
    
    return 0;
}