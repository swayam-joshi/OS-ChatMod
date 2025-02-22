/***************************************************
 * moderator.c
 ***************************************************/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <unistd.h>
#include <errno.h>

#define MAX_FILTERED 50
#define MAX_WORD_LEN 20
#define MAX_GROUPS 30
#define MAX_TEXT_SIZE 256

/* This matches the structure that group uses to send messages. */
typedef struct {
    long mtype;           // can be 1,2,3 or 30+group, etc.
    int timestamp;
    int user;
    char mtext[MAX_TEXT_SIZE];
    int modifyingGroup;
} Message;

/* For returning removal signals */
typedef struct {
    long mtype;  /* Use group_index+1 or something that group is receiving on */
    int group_id;
    int user_id;
    int removeUser;
} ModMessage;

/* Helper function: convert string to lowercase in-place */
void toLowerCase(char *str) {
    for(int i=0; str[i]; i++){
        str[i] = tolower((unsigned char)str[i]);
    }
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <testcase_number>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    /* Build path to input.txt to read keys (or you might re-read them again). 
       But the assignment says we only do: ./moderator.out X. The key is read from input.txt? 
       In real scenario, you'd parse input.txt again or get the key some other way.
       We'll assume the user gave us the same input again, or we just parse it as done in app. 
       For brevity, let's do it quickly: 
    */
    char testcase_folder[64];
snprintf(testcase_folder, sizeof(testcase_folder), "./testcase_%s", argv[1]);

    char input_file_path[128];
snprintf(input_file_path, sizeof(input_file_path), "%s/input.txt", testcase_folder);

    FILE *fp = fopen(input_file_path, "r");
    if (!fp) {
        perror("fopen input.txt in moderator");
        exit(EXIT_FAILURE);
    }
    int n, validation_key, app_key, moderator_key, violation_threshold;
    fscanf(fp, "%d", &n);
    fscanf(fp, "%d", &validation_key);
    fscanf(fp, "%d", &app_key);
    fscanf(fp, "%d", &moderator_key);
    fscanf(fp, "%d", &violation_threshold);
    /* skip group file names */
    for(int i=0; i<n; i++){
        char tmp[128];
        fscanf(fp, "%s", tmp); // discard
    }
    fclose(fp);

    /* We also must read filtered_words.txt to build a list of restricted words. */
    char filtered_path[128];
snprintf(filtered_path, sizeof(filtered_path), "%s/filtered_words.txt", testcase_folder);
    FILE *fwords = fopen(filtered_path, "r");
    if (!fwords) {
        perror("fopen filtered_words.txt");
        exit(EXIT_FAILURE);
    }
    char filtered_words[MAX_FILTERED][MAX_WORD_LEN+1];
    int filtered_count = 0;
    while (filtered_count < MAX_FILTERED && fscanf(fwords, "%s", filtered_words[filtered_count]) == 1) {
        /* convert each word to lowercase */
        toLowerCase(filtered_words[filtered_count]);
        filtered_count++;
    }
    fclose(fwords);

    /* Setup message queue for reading from groups */
    int mod_msqid = msgget(moderator_key, IPC_CREAT | 0666);
    if (mod_msqid < 0) {
        perror("msgget moderator");
        exit(EXIT_FAILURE);
    }

    /* We track violations per group+user: violations[group][user]. 
       But user can be up to 50. group up to 30 => 30x50 array 
    */
    static int violations[MAX_GROUPS][50];
    memset(violations, 0, sizeof(violations));

    /* Repeatedly read from queue until something ends. We'll break on error if 
       the queue is destroyed or we get an unexpected error. 
    */
    while(1) {
        Message msg;
        ssize_t rcv = msgrcv(mod_msqid, &msg, sizeof(msg) - sizeof(msg.mtype), 0 /* read any mtype */, 0);
        if (rcv < 0) {
            if (errno == EIDRM || errno == EINTR) {
                // The queue might have been removed => exit
                break;
            }
            if (errno == ENOMSG) {
                // No message => maybe sleep
                usleep(100000);
                continue;
            }
            perror("msgrcv in moderator");
            break;
        }

        /* Check if this is a group creation (mtype=1), user addition (mtype=2), group termination (mtype=3).
           Typically, we only care about actual user messages from group, i.e. (mtype = 30 + group#).
           We'll assume any large mtype means a user message. 
           For mtype=1,2,3, we can ignore. 
        */
        if (msg.mtype == 1 || msg.mtype == 2 || msg.mtype == 3) {
            // ignore
            continue;
        }

        /* Otherwise, it's presumably a user message. We do substring checks. */
        int g = msg.modifyingGroup;
        int u = msg.user;
        // Convert msg.mtext to lowercase:
        char textLower[MAX_TEXT_SIZE];
        strncpy(textLower, msg.mtext, sizeof(textLower)-1);
        textLower[MAX_TEXT_SIZE-1] = '\0';
        toLowerCase(textLower);

        /* Count how many *unique* filtered words appear. 
           We'll do a naive approach: for each filtered word, check if it is a substring of textLower. 
           If yes, increment local violation count. 
        */
        int localViolations = 0;
        for(int i=0; i<filtered_count; i++){
            if (strstr(textLower, filtered_words[i]) != NULL) {
                // found as substring => 1 violation for that word
                localViolations++;
            }
        }

        /* Update global violation count for (g,u) */
        violations[g][u] += localViolations;

        /* If >= threshold => remove user => send message to group. */
        if (violations[g][u] >= violation_threshold && localViolations>0) {
            // Print removal only once or every time? 
            // The assignment says: if user is to be deleted after crossing threshold -> print once.
            // We can check if exactly now crossed threshold:
            if (violations[g][u] - localViolations < violation_threshold) {
                /* user just crossed threshold => remove them */
                printf("User %d from group %d has been removed due to %d violations.\n",
                       u, g, violations[g][u]);

                /* Send removal message to group => use mtype = group_index+1 or similar. */
                ModMessage removeMsg;
                removeMsg.mtype = g+1;  // group listens on this type
                removeMsg.group_id = g;
                removeMsg.user_id = u;
                removeMsg.removeUser = 1;
                msgsnd(mod_msqid, &removeMsg, sizeof(removeMsg) - sizeof(removeMsg.mtype), 0);
            }
        }
    }

    return 0;
}