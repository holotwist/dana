#include "state.h"
#include "file_list.h"
#include "audio.h"
#include "ui.h"

#include <locale.h>
#include <unistd.h>
#include <stdio.h>
#include <pthread.h>

int main(int argc, char **argv) {
    setlocale(LC_ALL, ""); 
    if (argc > 1) { if (chdir(argv[1]) != 0) perror("chdir failed"); }
    load_directory(".");
    
    pthread_t audio_thread;
    pthread_create(&audio_thread, NULL, audio_thread_func, NULL);
    
    ui_run();
    
    pthread_join(audio_thread, NULL);
    DANAMetadata_Release(&p_header.metadata);
    return 0;
}