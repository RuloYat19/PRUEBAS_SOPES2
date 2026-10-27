'use strict';

const procesos = (() => {
	let listaProcesos = [];
	let activoActual = true;

	const cuerpo = document.getElementById('procesos-cuerpo');
	const aviso = document.getElementById('procesos-aviso');
	const filtroTexto = document.getElementById('filtro-texto');
	const interruptor = document.getElementById('interruptor-procesos');
	const estadoTexto = document.getElementById('estado-procesos');
	const panel = document.getElementById('panel-procesos');
	const avisoDesactivado = panel ? panel.querySelector('.aviso-desactivado') : null;

	const resTotal = document.getElementById('res-total');
	const resRunning = document.getElementById('res-running');
	const resSleeping = document.getElementById('res-sleeping');
	const resZombie = document.getElementById('res-zombie');

	function formatearMb(kb) {
		const mb = (Number(kb) || 0) / 1024;
		return mb.toFixed(1) + ' MB';
	}

	function mostrarAviso(mensaje, esError) {
		if (!aviso) return;
		aviso.textContent = mensaje;
		aviso.className = esError ? 'procesos-aviso error' : 'procesos-aviso exito';
	}

	function renderizar() {
		if (!cuerpo) return;
		while (cuerpo.firstChild) {
			cuerpo.removeChild(cuerpo.firstChild);
		}

		const termino = filtroTexto ? filtroTexto.value.trim().toLowerCase() : '';
		const esAdmin = auth.esAdmin();

		listaProcesos.forEach((proc) => {
			const pidStr = String(proc.pid);
			const nombreStr = proc.nombre || '';
			if (termino && !pidStr.includes(termino) && !nombreStr.toLowerCase().includes(termino)) {
				return;
			}

			const fila = document.createElement('tr');
			fila.setAttribute('data-pid', pidStr);

			fila.addEventListener('click', (e) => {
				if (e.target.tagName.toLowerCase() === 'button') {
					return;
				}
				if (typeof window.consultarPid === 'function' && auth.esAdmin()) {
					window.consultarPid(proc.pid);
				}
			});

			const tdPid = document.createElement('td');
			tdPid.textContent = pidStr;
			fila.appendChild(tdPid);

			const tdNombre = document.createElement('td');
			tdNombre.textContent = nombreStr;
			fila.appendChild(tdNombre);

			const tdEstado = document.createElement('td');
			tdEstado.textContent = proc.estado || '';
			fila.appendChild(tdEstado);

			const tdMemoria = document.createElement('td');
			tdMemoria.textContent = formatearMb(proc.rss_kb);
			fila.appendChild(tdMemoria);

			const tdPct = document.createElement('td');
			const pctVal = Number(proc.porcentaje_memoria) || 0;
			const contenedorBarra = document.createElement('div');
			contenedorBarra.className = 'barra-memoria-contenedor';
			const barra = document.createElement('div');
			barra.className = 'barra-memoria';
			barra.style.width = Math.min(100, Math.max(0, pctVal)) + '%';
			contenedorBarra.appendChild(barra);
			const spanPct = document.createElement('span');
			spanPct.className = 'porcentaje-texto';
			spanPct.textContent = pctVal.toFixed(2) + '%';
			contenedorBarra.appendChild(spanPct);
			tdPct.appendChild(contenedorBarra);
			fila.appendChild(tdPct);

			if (esAdmin) {
				const tdAccion = document.createElement('td');
				tdAccion.setAttribute('data-rol', 'admin_user');
				const btnKill = document.createElement('button');
				btnKill.type = 'button';
				btnKill.className = 'boton-terminar';
				btnKill.textContent = 'Terminar';
				btnKill.addEventListener('click', (e) => {
					e.stopPropagation();
					const confirmacion = confirm(
						'¿Terminar el proceso ' + proc.nombre + ' (PID ' + proc.pid + ')? Esta acción puede afectar la estabilidad del sistema.'
					);
					if (!confirmacion) return;

					api.post('/api/proceso/kill', { pid: proc.pid })
						.then((res) => {
							mostrarAviso('Proceso ' + (res.nombre || proc.nombre) + ' (PID ' + (res.pid || proc.pid) + ') terminado', false);
						})
						.catch((err) => {
							mostrarAviso(err.message || 'Error al terminar el proceso', true);
						});
				});
				tdAccion.appendChild(btnKill);
				fila.appendChild(tdAccion);
			}

			cuerpo.appendChild(fila);
		});
	}

	function aplicar(datos) {
		if (!datos) return;
		if (typeof datos.activo === 'boolean') {
			aplicarEstado(datos.activo);
		}
		if (datos.resumen) {
			if (resTotal) resTotal.textContent = datos.resumen.total !== undefined ? datos.resumen.total : '—';
			if (resRunning) resRunning.textContent = datos.resumen.running !== undefined ? datos.resumen.running : '—';
			if (resSleeping) resSleeping.textContent = datos.resumen.sleeping !== undefined ? datos.resumen.sleeping : '—';
			if (resZombie) resZombie.textContent = datos.resumen.zombie !== undefined ? datos.resumen.zombie : '—';
		}
		if (Array.isArray(datos.top)) {
			listaProcesos = datos.top;
			renderizar();
		}
	}

	function aplicarEstado(activo) {
		activoActual = !!activo;
		if (estadoTexto) {
			estadoTexto.textContent = activoActual ? 'Procesos: activos' : 'Procesos: desactivados';
		}
		if (interruptor && interruptor.checked !== activoActual) {
			interruptor.checked = activoActual;
		}
		if (avisoDesactivado) {
			avisoDesactivado.hidden = activoActual;
		}
	}

	function cargar() {
		api.get('/api/procesos')
			.then((resp) => resp.json())
			.then((datos) => {
				aplicar(datos);
			})
			.catch(() => {});
	}

	function conectarInterruptor() {
		if (interruptor) {
			interruptor.addEventListener('change', () => {
				const nuevoEstado = interruptor.checked;
				api.post('/api/monitoreo/procesos', { activo: nuevoEstado })
					.then((resp) => resp.json())
					.then((r) => {
						if (typeof r.procesos === 'boolean') {
							aplicarEstado(r.procesos);
						}
					})
					.catch((err) => {
						interruptor.checked = !nuevoEstado;
						mostrarAviso(err.message || 'Error al cambiar estado de escaneo', true);
					});
			});
		}

		if (filtroTexto) {
			filtroTexto.addEventListener('input', () => {
				renderizar();
			});
		}
	}

	return {
		cargar,
		aplicar,
		aplicarEstado,
		conectarInterruptor,
		renderizar
	};
})();
