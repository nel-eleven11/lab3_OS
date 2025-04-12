#define _GNU_SOURCE   // Para poder usar syscall(SYS_gettid) en algunas distribuciones
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <unistd.h>
#include <string.h>
#include <sys/syscall.h>   // Para syscall(SYS_gettid)
#include <pthread.h>
#include <sys/wait.h>
#include <errno.h>

// Si deseas usar OpenMP, inclúyelo (y compila con -fopenmp).
#ifdef _OPENMP
  #include <omp.h>
#endif

// Arreglo global 9x9 donde se cargará el Sudoku
static int sudoku[9][9];

// Variables globales para almacenar la validez de:
// filas, columnas y subarreglos.
static int validRows = 1;
static int validCols = 1;
static int validSubs = 1;

// ---------------------------------------------------------------------
// Función de utilidad: verifica que un arreglo de 9 enteros contenga
// todos los dígitos del 1 al 9 exactamente una vez.
int verifyDigits1to9(int arr[9]) {
    // Usamos un arreglo temporal de conteo (banderas)
    int found[10] = {0};  // found[d] = 1 si se encontró el dígito d

    for (int i = 0; i < 9; i++) {
        int val = arr[i];
        // Verifica que val esté entre 1 y 9
        if (val < 1 || val > 9) {
            return 0;
        }
        // Verifica que no se haya repetido
        if (found[val] == 1) {
            return 0;  // repetido
        }
        found[val] = 1;
    }

    // Si llegamos aquí, contiene los dígitos 1..9 sin repeticiones
    return 1;
}

// ---------------------------------------------------------------------
// Verifica si la fila 'rowIndex' del sudoku contiene 1..9
int checkRow(int rowIndex) {
    int row[9];
    for (int i = 0; i < 9; i++) {
        row[i] = sudoku[rowIndex][i];
    }
    return verifyDigits1to9(row);
}

// ---------------------------------------------------------------------
// Verifica si la columna 'colIndex' del sudoku contiene 1..9
int checkColumn(int colIndex) {
    int col[9];
    for (int i = 0; i < 9; i++) {
        col[i] = sudoku[i][colIndex];
    }
    return verifyDigits1to9(col);
}

// ---------------------------------------------------------------------
// Verifica un subarreglo 3x3 que comienza en (startRow, startCol)
int checkSubgrid(int startRow, int startCol) {
    int sub[9];
    int idx = 0;
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            sub[idx++] = sudoku[startRow + i][startCol + j];
        }
    }
    return verifyDigits1to9(sub);
}

// ---------------------------------------------------------------------
// Función que se ejecutará dentro del hilo (thread) para revisar todas
// las columnas usando checkColumn(...).
void* threadCheckColumns(void* arg) {
    // Muestra TID del thread que está corriendo esta función
    pid_t tid = (pid_t) syscall(SYS_gettid);
    printf("[HILO] Revisando columnas. TID del hilo = %d\n", tid);

    // Revisamos cada columna
    for (int c = 0; c < 9; c++) {
        if (!checkColumn(c)) {
            validCols = 0;
            break;
        }
    }

    pthread_exit(0);
}

// ---------------------------------------------------------------------
// Programa principal
int main(int argc, char* argv[]) {

    if (argc < 2) {
        fprintf(stderr, "Uso: %s <archivo_con_81_digitos>\n", argv[0]);
        return 1;
    }

    // 1) Abrir archivo y mapear a memoria
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
        fprintf(stderr, "El archivo no parece contener 81 caracteres.\n");
        close(fd);
        return 1;
    }

    char* fileMapping = (char*) mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (fileMapping == MAP_FAILED) {
        perror("mmap");
        close(fd);
        return 1;
    }

    close(fd); // Ya no se necesita el fd luego de mapear

    // 2) Copiar el contenido al arreglo sudoku[9][9]
    //    Asumimos que los primeros 81 bytes son dígitos del '1' al '9'
    int idx = 0;
    for (int i = 0; i < 9; i++) {
        for (int j = 0; j < 9; j++) {
            // Convertir el carácter '1'..'9' a entero 1..9
            sudoku[i][j] = fileMapping[idx++] - '0';
        }
    }

    // 3) Revisar subarreglos 3x3
    //    Se indican subcuadros cuyas esquinas superiores son (0,0), (0,3), (0,6),
    //    (3,0), (3,3), (3,6), (6,0), (6,3), (6,6).
    //    Usamos un for con pasos de 3.
#ifdef _OPENMP
    // Si deseas paralelizar con OpenMP la revisión de los subarreglos:
    #pragma omp parallel for default(none) shared(validSubs, sudoku) schedule(static)
#endif
    for (int sr = 0; sr < 9; sr += 3) {
        for (int sc = 0; sc < 9; sc += 3) {
            // Para evitar problemas de concurrencia en OpenMP, podrías usar
            // un if local; pero aquí lo simplificamos suponiendo que no hay
            // colisiones que arruinen la lógica.
            if (!checkSubgrid(sr, sc)) {
                validSubs = 0;
            }
        }
    }

    // 4) fork() -> en el hijo ejecutar `ps -p <pidPadre> -lLf`
    pid_t pidPadre = getpid();
    pid_t pidHijo1 = fork();
    if (pidHijo1 < 0) {
        perror("fork error");
        return 1;
    }
    else if (pidHijo1 == 0) {
        // Proceso hijo
        // Conocer PID del padre: getppid()
        // Pero lo que pide el enunciado es usar el PID del *padre original*,
        // que es pidPadre (obtenido antes del fork).
        char padreStr[32];
        sprintf(padreStr, "%d", pidPadre);

        // Ejecutar ps -p <pidPadre> -lLf
        execlp("ps", "ps", "-p", padreStr, "-lLf", (char *)NULL);

        // Si execlp falla:
        perror("execlp ps -p (primer fork)");
        exit(1);
    }
    // Proceso padre continúa

    // 5) Crear un pthread que revise todas las columnas
    pthread_t hiloCols;
    int err = pthread_create(&hiloCols, NULL, threadCheckColumns, NULL);
    if (err != 0) {
        fprintf(stderr, "Error al crear hilo: %s\n", strerror(err));
        return 1;
    }

    // Esperar a que termine el hilo
    pthread_join(hiloCols, NULL);

    // Mostrar el ID del thread principal que está en ejecución (el padre) en este momento
    pid_t threadId = syscall(SYS_gettid);
    printf("[PADRE] pthread_join() completado. TID del thread principal = %d\n", threadId);

    // Esperar a que el proceso hijo (que ejecuta ps) termine
    waitpid(pidHijo1, NULL, 0);

    // 6) Verificar filas (se hace en el proceso padre, secuencial)
    //    Si deseas, podrías usar OpenMP también aquí.
#ifdef _OPENMP
    #pragma omp parallel for default(none) shared(validRows, sudoku) schedule(static)
#endif
    for (int r = 0; r < 9; r++) {
        if (!checkRow(r)) {
            validRows = 0;
        }
    }

    // 7) Desplegar si la solución al Sudoku es válida o no
    //    Debe ser válida si filas, columnas y subarreglos son válidos.
    int solucionValida = (validRows && validCols && validSubs);
    if (solucionValida) {
        printf("[PADRE] La solución al Sudoku es VÁLIDA.\n");
    } else {
        printf("[PADRE] La solución al Sudoku es INVÁLIDA.\n");
    }

    // 8) Crear un nuevo fork() para ejecutar ps y comparar el número de LWP
    pid_t pidHijo2 = fork();
    if (pidHijo2 < 0) {
        perror("fork error");
        return 1;
    }
    else if (pidHijo2 == 0) {
        // Proceso hijo: ejecutar ps -p <pidPadre> -lLf
        char padreStr[32];
        sprintf(padreStr, "%d", pidPadre);

        execlp("ps", "ps", "-p", padreStr, "-lLf", (char*) NULL);

        perror("execlp ps -p (segundo fork)");
        exit(1);
    }

    // Proceso padre espera al segundo hijo
    waitpid(pidHijo2, NULL, 0);

    // Liberar mmap
    munmap(fileMapping, st.st_size);

    return 0;
}

