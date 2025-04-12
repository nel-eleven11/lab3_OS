#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <unistd.h>
#include <string.h>
#include <sys/syscall.h>
#include <pthread.h>
#include <sys/wait.h>
#include <errno.h>

// Incluir OpenMP (compilar con -fopenmp)
#ifdef _OPENMP
  #include <omp.h>
#endif

// Arreglo global 9x9
static int sudoku[9][9];

// Variables globales para la validez del Sudoku
static int validRows = 1;
static int validCols = 1;
static int validSubs = 1;

// --------------------------------------------------------------
// Verifica que un arreglo de 9 elementos contenga 1..9 sin repeticiones
int verifyDigits1to9(int arr[9]) {
    int found[10] = {0};
    for (int i = 0; i < 9; i++) {
        int val = arr[i];
        if (val < 1 || val > 9) {
            return 0;
        }
        if (found[val] == 1) {
            return 0; // repetido
        }
        found[val] = 1;
    }
    return 1;
}

// --------------------------------------------------------------
// Revisa la fila rowIndex
int checkRow(int rowIndex) {
    int row[9];
    for (int i = 0; i < 9; i++) {
        row[i] = sudoku[rowIndex][i];
    }
    return verifyDigits1to9(row);
}

// --------------------------------------------------------------
// Revisa la columna colIndex
int checkColumn(int colIndex) {
    int col[9];
    for (int i = 0; i < 9; i++) {
        col[i] = sudoku[i][colIndex];
    }
    return verifyDigits1to9(col);
}

// --------------------------------------------------------------
// Verifica un subarreglo 3x3 que comienza en (startRow, startCol)
int checkSubgrid(int startRow, int startCol) {
    int sub[9];
    int idx = 0;
    // Este bucle interior es muy corto (3x3=9 iteraciones);
    // normalmente no vale la pena paralelizarlo.
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            sub[idx++] = sudoku[startRow + i][startCol + j];
        }
    }
    return verifyDigits1to9(sub);
}

// --------------------------------------------------------------
// Función que se ejecutará en el hilo para revisar columnas
void* threadCheckColumns(void* arg) {
    pid_t tid = (pid_t) syscall(SYS_gettid);
    printf("[HILO] Revisando columnas. TID del hilo = %d\n", tid);
    int c; 

#ifdef _OPENMP
    // Paraleliza el bucle de 0..8 para columnas
    // Usamos 'private(c)' para que la variable c sea local a cada hilo
    // En caso de encontrar columna inválida, usamos #pragma omp atomic
    // para marcar validCols=0 de forma segura.
#pragma omp parallel for default(none) shared(validCols, sudoku) schedule(dynamic) private(c)
#endif
    for (int c = 0; c < 9; c++) {
        if (!checkColumn(c)) {
            // Si un hilo detecta columna inválida, pone validCols=0
#pragma omp atomic write 
            validCols = 0;
        }
    }

    pthread_exit(0);
}

// --------------------------------------------------------------
int main(int argc, char* argv[]) {

        // Establecer el número de hilos a 1 para OpenMP.
#ifdef _OPENMP
    omp_set_num_threads(1);
#endif

    if (argc < 2) {
        fprintf(stderr, "Uso: %s <archivo_con_81_digitos>\n", argv[0]);
        return 1;
    }

    // 1) Abrir y mapear archivo
    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) {
        perror("Error abriendo archivo");
        return 1;
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        perror("fstat");
        close(fd);
        return 1;
    }
    if (st.st_size < 81) {
        fprintf(stderr, "El archivo no contiene los 81 caracteres requeridos.\n");
        close(fd);
        return 1;
    }

    char* fileMapping = (char*) mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (fileMapping == MAP_FAILED) {
        perror("mmap");
        close(fd);
        return 1;
    }
    close(fd);

    // 2) Copiar contenido a sudoku[9][9]
    int idx = 0;
    for (int i = 0; i < 9; i++) {
        for (int j = 0; j < 9; j++) {
            sudoku[i][j] = fileMapping[idx++] - '0';
        }
    }

    // 3) Revisar subarreglos 3x3 (indices 0,3,6)
    // Podemos paralelizar el doble bucle sr=0..6(+=3), sc=0..6(+=3).
    // O usar 'collapse(2)' para fusionar niveles.
    // Cuidado: validSubs es compartida.
#ifdef _OPENMP
int sr, sc;
#pragma omp parallel for default(none) shared(validSubs, sudoku) schedule(dynamic) private(sr, sc) collapse(2)
#endif
    for (int sr = 0; sr < 9; sr += 3) {
        for (int sc = 0; sc < 9; sc += 3) {
            if (!checkSubgrid(sr, sc)) {
                // Marcamos validSubs=0 de forma segura:
#pragma omp atomic write 
                validSubs = 0;
            }
        }
    }

    // 4) fork para ejecutar ps -p <pidPadre> -lLf
    pid_t pidPadre = getpid();
    pid_t pidHijo1 = fork();
    if (pidHijo1 < 0) {
        perror("fork error");
        return 1;
    } else if (pidHijo1 == 0) {
        // Proceso hijo
        char padreStr[32];
        sprintf(padreStr, "%d", pidPadre);
        execlp("ps", "ps", "-p", padreStr, "-lLf", (char *)NULL);

        perror("execlp ps -p (primer fork)");
        exit(1);
    }
    // Proceso padre continua

    // 5) Crear un pthread que revise las columnas
    pthread_t hiloCols;
    int err = pthread_create(&hiloCols, NULL, threadCheckColumns, NULL);
    if (err != 0) {
        fprintf(stderr, "Error al crear hilo: %s\n", strerror(err));
        return 1;
    }

    // Esperar a que el hilo termine
    pthread_join(hiloCols, NULL);

    // Mostrar ID del thread principal (este)
    pid_t threadId = syscall(SYS_gettid);
    printf("[PADRE] pthread_join() completado. TID del thread principal = %d\n", threadId);

    // Esperar al proceso hijo que ejecuta ps
    waitpid(pidHijo1, NULL, 0);

    // 6) Revisar filas en el proceso padre
    // Paralelizamos el for de 0..8 para filas
#ifdef _OPENMP
int r;
#pragma omp parallel for default(none) shared(validRows, sudoku) schedule(dynamic) private(r)
#endif
    for (int r = 0; r < 9; r++) {
        if (!checkRow(r)) {
#pragma omp atomic write 
            validRows = 0;
        }
    }

    // 7) Validar solución
    int solucionValida = (validRows && validCols && validSubs);
    if (solucionValida) {
        printf("[PADRE] La solución al Sudoku es VÁLIDA.\n");
    } else {
        printf("[PADRE] La solución al Sudoku es INVÁLIDA.\n");
    }

    // 8) Segundo fork para ps -p <pidPadre> -lLf
    pid_t pidHijo2 = fork();
    if (pidHijo2 < 0) {
        perror("fork error");
        return 1;
    } else if (pidHijo2 == 0) {
        // Proceso hijo
        char padreStr[32];
        sprintf(padreStr, "%d", pidPadre);
        execlp("ps", "ps", "-p", padreStr, "-lLf", (char*) NULL);

        perror("execlp ps -p (segundo fork)");
        exit(1);
    }

    // Esperar al segundo hijo
    waitpid(pidHijo2, NULL, 0);

    // Liberar mmap
    munmap(fileMapping, st.st_size);

    return 0;
}
