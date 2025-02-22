#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <string.h>

#define MAX_GROUPS 30

typedef struct {
    long mtype;
    int group_id;
    char text[256];
} Message;

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <testcase_number>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    // Build path to input.txt
    char testcase_folder[50];
    snprintf(testcase_folder, sizeof(testcase_folder), "./testcase_%s", argv[1]);

    char input_file_path[128];
    snprintf(input_file_path, sizeof(input_file_path), "%s/input.txt", testcase_folder);

    // Read from input.txt
    FILE *fp = fopen(input_file_path, "r");
    if (!fp) {
        perror("Error opening input.txt");
        exit(EXIT_FAILURE);
    }

    int n, validation_key, app_key, moderator_key, violation_threshold;

    if (fscanf(fp, "%d %d %d %d %d", &n, &validation_key, &app_key, &moderator_key, &violation_threshold) != 5) {
        fprintf(stderr, "Error reading input.txt: Invalid format\n");
        fclose(fp);
        exit(EXIT_FAILURE);
    }

    if (n > MAX_GROUPS) {
        fprintf(stderr, "Number of groups exceeds maximum supported.\n");
        fclose(fp);
        exit(EXIT_FAILURE);
    }

    // Read group file paths and verify existence
char group_files[MAX_GROUPS][256]; // Increased buffer size
    for (int i = 0; i < n; i++) {
        char group_file_name[128];
        if (fscanf(fp, "%s", group_file_name) != 1) {
            fprintf(stderr, "Error reading group file path from input.txt\n");
            fclose(fp);
            exit(EXIT_FAILURE);
        }
int written = snprintf(group_files[i], sizeof(group_files[i]), "testcase_%s/%s", argv[1], group_file_name);
if (written < 0 || written >= sizeof(group_files[i])) {
    fprintf(stderr, "Error: Group file path too long, truncation occurred.\n");
    fclose(fp);
    exit(EXIT_FAILURE);
}

        // Verify file existence
        FILE *test_fp = fopen(group_files[i], "r");
        if (!test_fp) {
            fprintf(stderr, "Error: Group file '%s' does not exist\n", group_files[i]);
            fclose(fp);
            exit(EXIT_FAILURE);
        }
        fclose(test_fp);
    }
    fclose(fp);

    int msgid = msgget(app_key, IPC_CREAT | 0666);
    if (msgid == -1) {
        perror("msgget failed");
        exit(EXIT_FAILURE);
    }

    /* Spawn each group */
    pid_t group_pids[MAX_GROUPS];
    for (int i = 0; i < n; i++) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork failed");
            exit(EXIT_FAILURE);
        } else if (pid == 0) { // Child process
            char group_index_str[10];
            snprintf(group_index_str, sizeof(group_index_str), "%d", i);
            char val_key_str[20], app_key_str[20], mod_key_str[20], viol_str[20];
            snprintf(val_key_str, sizeof(val_key_str), "%d", validation_key);
            snprintf(app_key_str, sizeof(app_key_str), "%d", app_key);
            snprintf(mod_key_str, sizeof(mod_key_str), "%d", moderator_key);
            snprintf(viol_str, sizeof(viol_str), "%d", violation_threshold);

            execl("./groups.out", 
                  "groups.out",
                  group_files[i], // Now contains full path
                  group_index_str,
                  argv[1],
                  val_key_str,
                  app_key_str,
                  mod_key_str,
                  viol_str,
                  (char*)NULL);

            /* If execl fails */
            perror("execl failed to execute groups.out");
            exit(EXIT_FAILURE);
        } else {
            /* Parent just stores PID */
            group_pids[i] = pid;
        }

        // sleep(1);
        printf("Spawned group %d\n", i);
    }

    int active_groups = n;
    while (active_groups > 0) {
        Message msg;
        if (msgrcv(msgid, &msg, sizeof(msg) - sizeof(long), 3, 0) > 0) {
            printf("All users terminated. Exiting group process %d.\n", msg.group_id);
            active_groups--;
        }
    }

    msgctl(msgid, IPC_RMID, NULL);

    return 0;
}
