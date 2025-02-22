/***************************************************
 * groups.c
 ***************************************************/
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/types.h>
#include <errno.h>
#include <string.h>
#include <sys/wait.h>
#include <fcntl.h>

#define MAX_USERS 50
#define MAX_GROUPS 30
#define MAX_TEXT_SIZE 256

typedef struct {
    long mtype;
    int timestamp;
    int user;
    char mtext[256];
    int modifyingGroup;
} Message;

/* For communication from group to moderator, or vice versa */
typedef struct {
    long mtype;        /* Could be the group ID or some known type */
    int group_id;
    int user_id;
    int removeUser;    /* 1 if user is to be removed, 0 otherwise */
} ModMessage;

static int group_user_count[MAX_GROUPS] = {0};  // Track users per group

int main(int argc, char *argv[]) {
    if (argc != 8) {
        /* Expecting:
           1) group_file (e.g. groups/group_0.txt)
           2) group_index
           3) testcase_number
           4) validation_key
           5) app_key
           6) moderator_key
           7) violation_threshold
        */
        fprintf(stderr, "Usage: %s <group_file> <group_index> <testcase> <valKey> <appKey> <modKey> <violThreshold>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    char *group_file = argv[1];
    int group_index = atoi(argv[2]);
    char *testcase_number = argv[3]; // Assuming argv[3] is the testcase number
    int validation_key = atoi(argv[4]);
    int app_key = atoi(argv[5]);
    int moderator_key = atoi(argv[6]);
    int violation_threshold = atoi(argv[7]);

    /* Open group file and read #users + user file paths */
    FILE *gf = fopen(group_file, "r");
    if (!gf) {
        perror("fopen group_file");
        exit(EXIT_FAILURE);
    }

    int initial_users;
    fscanf(gf, "%d", &initial_users);
    char user_files[MAX_USERS][128];
    for(int i = 0; i < initial_users; i++){
        fscanf(gf, "%s", user_files[i]);
        char user_file_path[256];
snprintf(user_file_path, sizeof(user_file_path), "testcase_%s/%s", testcase_number, user_files[i]);
        printf("Attempting to open user file: %s\n", user_file_path); // Debugging output
    }
    fclose(gf);

    /* ========== CREATE MESSAGE QUEUES ========== */
    /* The group needs to send messages to the validation queue, so get its ID: */
    int val_msqid = msgget(validation_key, 0666);
    if (val_msqid < 0) {
        perror("msgget validation");
        exit(EXIT_FAILURE);
    }

    if (val_msqid == -1) {
        fprintf(stderr, "Error: Message queue for validation failed\n");
        exit(EXIT_FAILURE);
    }

    /* The group sends messages to the moderator, and also receives from it: */
    int mod_msqid = msgget(moderator_key, 0666);
    if (mod_msqid < 0) {
        perror("msgget moderator");
        exit(EXIT_FAILURE);
    }

    /* The group may optionally communicate with the app via a queue: */
    int app_msqid = msgget(app_key, 0666);
    if (app_msqid < 0) {
        perror("msgget app");
        // not necessarily fatal—depends on your design
        exit(EXIT_FAILURE);
    }

    /* ========== NOTIFY VALIDATION: GROUP CREATED (mtype = 1) ========== */
    Message createMsg;
    createMsg.mtype = 1;
    createMsg.timestamp = 0;   // ignored
    createMsg.user = 0;        // ignored
    createMsg.mtext[0] = '\0'; // ignored
    createMsg.modifyingGroup = group_index;

    if (msgsnd(val_msqid, &createMsg, sizeof(createMsg) - sizeof(createMsg.mtype), 0) == -1) {
        perror("msgsnd group creation");
        // If validation fails, might as well exit
        exit(EXIT_FAILURE);
    }

    /* We also maintain arrays to track pipe fds and user statuses. */
    int active_users = initial_users;
    int user_removed_count = 0;  // how many were removed for violations
    pid_t user_pids[MAX_USERS];
    int pipes[MAX_USERS][2];     // each user has a pipe to the group

    /* ========== CREATE USER PROCESSES ========== */
    for(int i = 0; i < initial_users; i++){
        /* Create pipe for user i -> group */
        if (pipe(pipes[i]) < 0) {
            perror("pipe");
            exit(EXIT_FAILURE);
        }

        pid_t cpid = fork();
        if (cpid < 0) {
            perror("fork user");
            exit(EXIT_FAILURE);
        }
        else if (cpid == 0) {
            /* CHILD = user process */
            close(pipes[i][0]); // child won't read from pipe, only write

            /* Open user's file and read lines (timestamp + message) */
            char user_file_path[256];
            snprintf(user_file_path, sizeof(user_file_path), "testcase_%s/%s", testcase_number, user_files[i]);
            printf("Attempting to open user file: %s\n", user_file_path); // Debugging output

            FILE *uf = fopen(user_file_path, "r");
            if (!uf) {
                fprintf(stderr, "Error opening user file: %s\n", user_file_path);
                perror("fopen user_file");
                exit(EXIT_FAILURE);
            }
            int timestamp;
            char text[MAX_TEXT_SIZE];
            /* For each line, read <timestamp> <message> */
            while (fscanf(uf, "%d %s", &timestamp, text) == 2) {
                /* We must write timestamp & text into pipe in a structured way. 
                   We must ensure we write exactly 256 bytes each time (for boundary). */
char buffer[MAX_TEXT_SIZE + 50]; // Ensure extra space for formatting
memset(buffer, 0, sizeof(buffer)); // Clear buffer to prevent garbage data

                /* Format: timestamp user_index actual_text ... 
                   or we can store them as a small struct. We'll do simple text: 
                   e.g. "timestamp userIndex text" 
                */
char safe_text[MAX_TEXT_SIZE];
strncpy(safe_text, text, sizeof(safe_text) - 1); // Ensure null-termination
safe_text[sizeof(safe_text) - 1] = '\0'; // Null-terminate to prevent overflow

int written = snprintf(buffer, sizeof(buffer), "%d %d %s", timestamp, i, safe_text);
if (written < 0 || written >= sizeof(buffer)) {
    fprintf(stderr, "Error: Message formatting may be truncated.\n");
    exit(EXIT_FAILURE);
}

                /* Write to pipe */
                if (write(pipes[i][1], buffer, sizeof(buffer)) < 0) {
                    perror("write to pipe");
                }
                /* Sleep or flush if needed. We can do a small usleep to avoid flooding. */
                usleep(5000);
            }
            fclose(uf);

            /* Once done sending, close the write end. This signals the group process that no more data. */
            close(pipes[i][1]);
            exit(0);
        }
        else {
            /* PARENT (group) side */
            close(pipes[i][1]); // group won't write to the pipe, only read
            user_pids[i] = cpid;

            /* ========== NOTIFY VALIDATION: NEW USER (mtype = 2) ========== */
            Message userMsg;
            userMsg.mtype = 2;
            userMsg.timestamp = 0; // ignored
            userMsg.user = i;      // user index
            memset(userMsg.mtext, 0, sizeof(userMsg.mtext));
            userMsg.modifyingGroup = group_index;

            if (group_user_count[group_index] >= MAX_USERS) {
                fprintf(stderr, "Error: Cannot add more users to group %d (limit reached)\n", group_index);
                exit(EXIT_FAILURE);
            }

            if (msgsnd(val_msqid, &userMsg, sizeof(userMsg) - sizeof(userMsg.mtype), 0) == -1) {
                perror("msgsnd new user");
                exit(EXIT_FAILURE);
            }

            group_user_count[group_index]++;  // Increase user count
        }
    }

    /* ========== READ MESSAGES FROM USERS, FORWARD TO VALIDATION & MODERATOR ========== */
    /* We must read from multiple pipes simultaneously. For a simplified approach, we'll do blocking reads
       in a round-robin manner. A real solution might use select/poll or a priority queue by timestamp. */

    int users_active[MAX_USERS];
    memset(users_active, 1, sizeof(users_active));

    int total_active = active_users;

    while (1) {
        /* If we have fewer than 2 active users, terminate the group. */
        if (total_active < 2) {
            break;
        }

        /* Attempt to read from each active user pipe.
           If there's data, parse it, send to validation + moderator.
           If EOF, that user is done => user becomes inactive => decrement total_active.
        */

        int all_empty = 1; // track if no user provided new data in a pass
        for (int i = 0; i < initial_users; i++) {
            if (!users_active[i]) continue; // already inactive/removed

            char buffer[256];
            memset(buffer, 0, sizeof(buffer));

            /* Non-blocking or timed blocking read is best. For simplicity, let's do a read with O_NONBLOCK:
               We'll set the pipe to non-blocking, or just do a small read check. 
               This is highly simplified.
            */
            ssize_t r = read(pipes[i][0], buffer, sizeof(buffer));
            if (r > 0) {
                all_empty = 0;
                /* Parse the buffer for timestamp, user, text */
                int ts, usr;
                char msgText[256];
                memset(msgText, 0, sizeof(msgText));
if (sscanf(buffer, "%d %d %255s", &ts, &usr, msgText) != 3) {
    fprintf(stderr, "Error parsing message from user %d: %s\n", usr, buffer);
    continue;
}

                /* (E) Send chat to validation */
                Message chatMsg;
                chatMsg.mtype = MAX_GROUPS + group_index; // e.g., 30 + group_index
                chatMsg.timestamp = ts;
                chatMsg.user = usr;
                strncpy(chatMsg.mtext, msgText, sizeof(chatMsg.mtext)-1);
                chatMsg.modifyingGroup = group_index;

                if (chatMsg.mtype <= 0) {
                    fprintf(stderr, "Error: Invalid message type (%ld)\n", chatMsg.mtype);
                    exit(EXIT_FAILURE);
                }

                if (msgsnd(val_msqid, &chatMsg, sizeof(chatMsg) - sizeof(long), 0) == -1) {
                    perror("msgsnd chat message");
                    exit(EXIT_FAILURE);
                }

                /* (F) Also send to moderator */
                /* We could send the same structure, or a simpler one. Let's reuse Message for demonstration. */
                msgsnd(mod_msqid, &chatMsg, sizeof(chatMsg) - sizeof(chatMsg.mtype), 0);

            }
            else if (r == 0) {
                /* Pipe closed -> user done => user leaves group. */
                users_active[i] = 0;
                total_active--;
            }
            else {
                /* r < 0 => maybe EAGAIN or EWOULDBLOCK or error.
                   We skip it for now if it's just EAGAIN. 
                */
            }
        }

        /* (G) Check if moderator wants to remove any user. 
           We could do a non-blocking msgrcv on mod_msqid to see if there's a removal message. 
           We'll do a small while loop to drain. This is a simplistic approach.
        */
        while (1) {
            ModMessage m;
            ssize_t rc = msgrcv(mod_msqid, &m, sizeof(m) - sizeof(m.mtype), group_index+1, IPC_NOWAIT);
            if (rc < 0) {
                if (errno == ENOMSG) {
                    // no more messages for us
                    break;
                } else {
                    // Another error
                    break;
                }
            }
            /* If removeUser == 1, remove that user from group. */
            if (m.removeUser == 1 && users_active[m.user_id]) {
                // close pipe read end
                close(pipes[m.user_id][0]);
                users_active[m.user_id] = 0;
                total_active--;
                user_removed_count++;
            }
        }

        if (all_empty) {
            /* Means no new data from any user. Sleep to avoid busy loop. */
            usleep(50000);
        }
    }

    /* ========== GROUP TERMINATION (H) ========== */
    /* (I) mtype = 3 to validation */
    Message termMsg;
    termMsg.mtype = 3;
    termMsg.timestamp = 0;
    termMsg.user = user_removed_count; // per requirement: # users removed due to violations
    termMsg.mtext[0] = '\0';
    termMsg.modifyingGroup = group_index;

    msgsnd(val_msqid, &termMsg, sizeof(termMsg) - sizeof(termMsg.mtype), 0);

    /* Cleanup: kill or wait for any user processes still running, if they haven't exited. */
    for (int i = 0; i < initial_users; i++) {
        if (waitpid(user_pids[i], NULL, WNOHANG) == 0) {
            // user is still alive => wait
            waitpid(user_pids[i], NULL, 0);
        }
    }

    /* Optionally notify the app if needed. Some designs do:
       AppMessage finishMsg;
       finishMsg.mtype = 999; // or group_index + 1000, etc.
       finishMsg.group_id = group_index;
       strcpy(finishMsg.text, "DONE");
       msgsnd(app_msqid, &finishMsg, sizeof(finishMsg)-sizeof(finishMsg.mtype), 0);
    */

    return 0;
}