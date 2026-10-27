'use strict';

const archivos = (() => {
	let listaArchivos = [];

	const cuerpo = document.getElementById('archivos-cuerpo');
	const interruptor = document.getElementById('interruptor-archivos');
	const estadoTexto = document.getElementById('estado-archivos');
	const rutaActual = document.getElementById('ruta-actual');
	const sensorError = document.getElementById('sensor-error');
	const aviso = document.getElementById('archivos-aviso');
	const formRuta = document.getElementById('form-ruta');
	const inputRuta = document.getElementById('ruta-vigilada');
	const panel = document.getElementById('panel-archivos');
	const avisoDesactivado = panel ? panel.querySelector('.aviso-desactivado') : null;

	function renderizar() {
		if (!cuerpo) return;
		while (cuerpo.firstChild) {
			cuerpo.removeChild(cuerpo.firstChild);
		}

		listaArchivos.forEach((a) => {
			const tr = document.createElement('tr');

			const tdArchivo = document.createElement('td');
			tdArchivo.textContent = a.nombre || a.ruta;
			tdArchivo.title = a.ruta;

			const tdEstado = document.createElement('td');
			const spanEstado = document.createElement('span');
			spanEstado.className = 'estado-tag ' + (a.estado || 'benigno');
			if (a.estado === 'infectado' && a.firma) {
				spanEstado.textContent = `infectado (${a.firma})`;
			} else {
				spanEstado.textContent = a.estado || 'benigno';
			}
			tdEstado.appendChild(spanEstado);

			if (a.cuarentena) {
				const marcaCuarentena = document.createElement('span');
				marcaCuarentena.className = 'marca-cuarentena';
				marcaCuarentena.textContent = ' [en cuarentena]';
				tdEstado.appendChild(marcaCuarentena);
			}

			const tdMtime = document.createElement('td');
			tdMtime.textContent = a.mtime || '—';

			const tdSha = document.createElement('td');
			const spanSha = document.createElement('span');
			spanSha.className = 'hash-sha256';
			spanSha.title = a.sha256 || '';
			spanSha.textContent = (a.sha256 && a.sha256.length >= 12) ? a.sha256.substring(0, 12) : (a.sha256 || '—');
			tdSha.appendChild(spanSha);

			const tdEvento = document.createElement('td');
			if (a.pid > 0) {
				tdEvento.textContent = `${a.accion} · PID ${a.pid} (${a.proceso})`;
			} else {
				tdEvento.textContent = a.accion || 'inicial';
			}

			const tdAccion = document.createElement('td');
			tdAccion.dataset.rol = 'admin_user';
			if (window.auth && !window.auth.esAdmin()) {
				tdAccion.style.display = 'none';
			}
			const botonCuarentena = document.createElement('button');
			botonCuarentena.className = 'boton-cuarentena';
			botonCuarentena.textContent = 'Cuarentena';
			if (a.cuarentena) {
				botonCuarentena.disabled = true;
			}
			botonCuarentena.addEventListener('click', async () => {
				const nombre = a.nombre || a.ruta;
				if (!confirm(`¿Poner en cuarentena ${nombre}? El archivo quedará con permisos 0000.`)) {
					return;
				}
				try {
					const r = await api.post('/api/cuarentena', { ruta: a.ruta });
					const d = await r.json().catch(() => ({}));
					if (!r.ok) {
						if (aviso) {
							aviso.textContent = d.error || 'No se pudo poner en cuarentena';
							aviso.className = 'archivos-aviso error';
						}
						return;
					}
					a.cuarentena = true;
					renderizar();
					if (aviso) {
						aviso.textContent = `Archivo puesto en cuarentena: ${a.ruta}`;
						aviso.className = 'archivos-aviso exito';
					}
					if (typeof antivirus !== 'undefined' && typeof antivirus.cargarCuarentena === 'function') {
						antivirus.cargarCuarentena();
					}
				} catch (err) {
					if (aviso) {
						aviso.textContent = 'Error de conexión con el daemon';
						aviso.className = 'archivos-aviso error';
					}
				}
			});
			tdAccion.appendChild(botonCuarentena);

			tr.appendChild(tdArchivo);
			tr.appendChild(tdEstado);
			tr.appendChild(tdMtime);
			tr.appendChild(tdSha);
			tr.appendChild(tdEvento);
			tr.appendChild(tdAccion);

			cuerpo.appendChild(tr);
		});
	}

	function aplicarLista(d) {
		if (!d) return;
		if (typeof d.activo === 'boolean') {
			if (interruptor)
				interruptor.checked = d.activo;
			if (estadoTexto)
				estadoTexto.textContent = d.activo ? 'Archivos: activo' : 'Archivos: desactivado';
			if (avisoDesactivado)
				avisoDesactivado.hidden = !!d.activo;
		}
		if (d.ruta && rutaActual) {
			rutaActual.textContent = d.ruta;
		}
		if (Array.isArray(d.archivos)) {
			listaArchivos = d.archivos.slice(0, 256);
			listaArchivos.sort((x, y) => (y.analizado || '').localeCompare(x.analizado || ''));
			renderizar();
		}
	}

	function aplicarArchivo(a) {
		if (!a || !a.ruta) return;
		const idx = listaArchivos.findIndex((item) => item.ruta === a.ruta);
		if (idx >= 0) {
			listaArchivos[idx] = a;
		} else {
			listaArchivos.push(a);
		}
		listaArchivos.sort((x, y) => (y.analizado || '').localeCompare(x.analizado || ''));
		if (listaArchivos.length > 256) {
			listaArchivos = listaArchivos.slice(0, 256);
		}
		renderizar();
	}

	function aplicarEstado(m) {
		if (!m) return;
		if (typeof m.archivos === 'boolean') {
			if (interruptor)
				interruptor.checked = m.archivos;
			if (estadoTexto)
				estadoTexto.textContent = m.archivos ? 'Archivos: activo' : 'Archivos: desactivado';
			if (avisoDesactivado)
				avisoDesactivado.hidden = !!m.archivos;
		}
		if (typeof m.ruta_vigilada === 'string' && rutaActual) {
			rutaActual.textContent = m.ruta_vigilada;
		}
		if (sensorError) {
			if (m.sensor_error) {
				sensorError.textContent = m.sensor_error;
				sensorError.hidden = false;
			} else {
				sensorError.textContent = '';
				sensorError.hidden = true;
			}
		}
	}

	function conectarControles() {
		if (interruptor) {
			interruptor.addEventListener('change', async () => {
				const deseado = interruptor.checked;
				try {
					const r = await api.post('/api/monitoreo/archivos', { activo: deseado });
					const d = await r.json().catch(() => ({}));
					if (!r.ok) {
						interruptor.checked = !deseado;
						if (aviso) {
							aviso.textContent = d.error || 'No se pudo cambiar el escaneo de archivos';
							aviso.className = 'archivos-aviso error';
						}
						return;
					}
					if (d && typeof d.archivos === 'boolean') {
						interruptor.checked = d.archivos;
						if (estadoTexto)
							estadoTexto.textContent = d.archivos ? 'Archivos: activo' : 'Archivos: desactivado';
						if (avisoDesactivado)
							avisoDesactivado.hidden = !!d.archivos;
					}
					if (aviso) {
						aviso.textContent = '';
						aviso.className = 'archivos-aviso';
					}
				} catch (e) {
					interruptor.checked = !deseado;
					if (aviso) {
						aviso.textContent = 'Error de conexión con el daemon';
						aviso.className = 'archivos-aviso error';
					}
				}
			});
		}

		if (formRuta) {
			formRuta.addEventListener('submit', async (ev) => {
				ev.preventDefault();
				if (!inputRuta) return;
				const ruta = inputRuta.value.trim();
				if (!ruta) return;
				try {
					const r = await api.post('/api/ruta', { ruta });
					const d = await r.json().catch(() => ({}));
					if (!r.ok) {
						if (aviso) {
							aviso.textContent = d.error || 'No se pudo cambiar la ruta';
							aviso.className = 'archivos-aviso error';
						}
						return;
					}
					inputRuta.value = '';
					if (d.ruta && rutaActual) {
						rutaActual.textContent = d.ruta;
					}
					if (aviso) {
						aviso.textContent = d.cambio ? 'Ruta vigilada cambiada a ' + d.ruta : 'La ruta no cambió';
						aviso.className = 'archivos-aviso exito';
					}
				} catch (e) {
					if (aviso) {
						aviso.textContent = 'Error de conexión con el daemon';
						aviso.className = 'archivos-aviso error';
					}
				}
			});
		}
	}

	function cargar() {
		api.get('/api/archivos')
			.then((r) => r.json())
			.then((d) => {
				aplicarLista(d);
			})
			.catch(() => {});
	}

	return {
		cargar,
		renderizar,
		aplicarLista,
		aplicarArchivo,
		aplicarEstado,
		conectarControles
	};
})();
