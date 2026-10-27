/*
 * ============================================================
 * DASHBOARD PRACTICA 4 - SO2
 * ============================================================
 */

let currentProcesses = [];


/* ============================================================
 * UTILIDADES
 * ============================================================ */

function formatKB(kb) {

    if (kb < 1024) {
        return `${kb} KB`;
    }

    const mb = kb / 1024;

    if (mb < 1024) {
        return `${mb.toFixed(1)} MB`;
    }

    const gb = mb / 1024;

    return `${gb.toFixed(2)} GB`;
}


function formatRuntime(ms) {

    if (ms < 1000) {
        return `${ms} ms`;
    }

    const seconds = ms / 1000;

    if (seconds < 60) {
        return `${seconds.toFixed(1)} s`;
    }

    const minutes = seconds / 60;

    return `${minutes.toFixed(1)} min`;
}


/* ============================================================
 * ESTADO DEL SERVIDOR
 * ============================================================ */

function setConnectionStatus(online) {

    const dot =
        document.getElementById(
            "statusDot"
        );

    const text =
        document.getElementById(
            "statusText"
        );

    if (online) {

        dot.className =
            "status-dot online";

        text.textContent =
            "Daemon conectado";

    } else {

        dot.className =
            "status-dot offline";

        text.textContent =
            "Daemon desconectado";
    }
}


/* ============================================================
 * ACTUALIZAR MEMORIA
 * ============================================================ */

function updateMemory(memory) {

    document.getElementById(
        "memoryTotal"
    ).textContent =
        formatKB(memory.total_kb);


    document.getElementById(
        "memoryUsed"
    ).textContent =
        formatKB(memory.used_kb);


    document.getElementById(
        "memoryFree"
    ).textContent =
        formatKB(memory.free_kb);


    document.getElementById(
        "memoryCache"
    ).textContent =
        formatKB(memory.cache_kb);


    document.getElementById(
        "swapUsed"
    ).textContent =
        formatKB(memory.swap_used_kb);


    document.getElementById(
        "swapTotal"
    ).textContent =
        formatKB(memory.swap_total_kb);


    let percentage = 0;

    if (memory.total_kb > 0) {

        percentage =
            (
                memory.used_kb /
                memory.total_kb
            ) * 100;
    }


    document.getElementById(
        "memoryBar"
    ).style.width =
        `${percentage.toFixed(1)}%`;


    document.getElementById(
        "memoryPercentage"
    ).textContent =
        `${percentage.toFixed(1)}%`;
}


/* ============================================================
 * ESTADO DE PROCESOS
 * ============================================================ */

function updateProcessState(state) {

    document.getElementById(
        "processTotal"
    ).textContent =
        state.total;


    document.getElementById(
        "processRunning"
    ).textContent =
        state.running;


    document.getElementById(
        "processSleeping"
    ).textContent =
        state.sleeping;


    document.getElementById(
        "processZombie"
    ).textContent =
        state.zombie;
}


/* ============================================================
 * TABLA DE PROCESOS
 * ============================================================ */

function updateProcessTable() {

    const table =
        document.getElementById(
            "processTable"
        );


    const search =
        document.getElementById(
            "pidSearch"
        ).value
        .trim()
        .toLowerCase();


    let filtered =
        currentProcesses;


    if (search !== "") {

        filtered =
            currentProcesses.filter(
                process => {

                    return (
                        String(process.pid)
                            .includes(search)
                        ||
                        process.name
                            .toLowerCase()
                            .includes(search)
                    );
                }
            );
    }


    if (filtered.length === 0) {

        table.innerHTML = `
            <tr>
                <td colspan="4">
                    No se encontraron procesos.
                </td>
            </tr>
        `;

        return;
    }


    /*
     * Ordenar por memoria descendente.
     */
    filtered =
        [...filtered].sort(
            (a, b) =>
                b.memory_kb -
                a.memory_kb
        );


    /*
     * Mostrar máximo 100 en pantalla.
     */
    filtered =
        filtered.slice(0, 100);


    table.innerHTML =
        filtered.map(process => {

            return `
                <tr>

                    <td>
                        ${process.pid}
                    </td>

                    <td>
                        ${escapeHtml(
                            process.name
                        )}
                    </td>

                    <td>
                        ${formatRuntime(
                            process.runtime_ms
                        )}
                    </td>

                    <td>
                        ${formatKB(
                            process.memory_kb
                        )}
                    </td>

                </tr>
            `;

        }).join("");
}


/* ============================================================
 * EVENTOS DE SEGURIDAD
 * ============================================================ */

function updateSecurityEvents(events) {

    const container =
        document.getElementById(
            "securityEvents"
        );


    const count =
        document.getElementById(
            "eventCount"
        );


    count.textContent =
        `${events.length} evento${
            events.length === 1
                ? ""
                : "s"
        }`;


    if (events.length === 0) {

        container.innerHTML = `
            <div class="empty-event">
                No hay eventos de seguridad.
            </div>
        `;

        return;
    }


    /*
     * Mostrar primero los eventos más recientes.
     */
    const reversed =
        [...events].reverse();


    container.innerHTML =
        reversed.map(event => {

            return `
                <div class="security-event">

                    <div
                        class="security-event-header"
                    >

                        <span
                            class="security-event-title"
                        >
                            ⚠ ${escapeHtml(
                                event.palabra
                            )}
                        </span>

                        <span
                            class="security-event-time"
                        >
                            ${escapeHtml(
                                event.timestamp
                            )}
                        </span>

                    </div>


                    <div
                        class="security-event-file"
                    >
                        <strong>
                            Archivo:
                        </strong>

                        ${escapeHtml(
                            event.archivo
                        )}
                    </div>


                    <div
                        class="security-event-content"
                    >
                        ${escapeHtml(
                            event.contenido
                        )}
                    </div>

                </div>
            `;

        }).join("");
}


/* ============================================================
 * ESCAPAR HTML
 * ============================================================ */

function escapeHtml(value) {

    return String(value)
        .replaceAll("&", "&amp;")
        .replaceAll("<", "&lt;")
        .replaceAll(">", "&gt;")
        .replaceAll('"', "&quot;")
        .replaceAll("'", "&#039;");
}


/* ============================================================
 * OBTENER DATOS DEL DAEMON
 * ============================================================ */

async function updateDashboard() {

    try {

        const response =
            await fetch(
                "/api/status",
                {
                    cache: "no-store"
                }
            );


        if (!response.ok) {
            throw new Error(
                `HTTP ${response.status}`
            );
        }


        const data =
            await response.json();


        setConnectionStatus(true);


        updateMemory(
            data.memory
        );


        updateProcessState(
            data.processes_state
        );


        currentProcesses =
            data.processes || [];


        updateProcessTable();


        updateSecurityEvents(
            data.security_events || []
        );


        document.getElementById(
            "memoryUpdated"
        ).textContent =
            `Actualizado: ${
                new Date().toLocaleTimeString()
            }`;


    } catch (error) {

        console.error(
            "Error conectando con daemon:",
            error
        );


        setConnectionStatus(false);


        document.getElementById(
            "memoryUpdated"
        ).textContent =
            "Sin conexión";
    }
}


/* ============================================================
 * BUSQUEDA DE PROCESOS
 * ============================================================ */

document.getElementById(
    "pidSearch"
).addEventListener(
    "input",
    updateProcessTable
);


/* ============================================================
 * INICIO
 * ============================================================ */

updateDashboard();


/*
 * Actualizar cada 3 segundos.
 */
setInterval(
    updateDashboard,
    3000
);