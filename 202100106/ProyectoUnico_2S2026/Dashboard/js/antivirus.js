'use strict';

const antivirus = (() => {
	let listaAmenazas = [];
	let listaCuarentena = [];

	const cuerpoAmenazas = document.getElementById('amenazas-cuerpo');
	const cuerpoCuarentena = document.getElementById('cuarentena-cuerpo');
	const nombreBlacklist = document.getElementById('blacklist-nombre');
	const versionBlacklist = document.getElementById('blacklist-version');
	const fechaBlacklist = document.getElementById('blacklist-fecha');
	const cantidadBlacklist = document.getElementById('blacklist-cantidad');
	const errorBlacklist = document.getElementById('blacklist-error');
	const formBlacklist = document.getElementById('form-blacklist');
	const inputBlacklist = document.getElementById('archivo-blacklist');
	const avisoBlacklist = document.getElementById('blacklist-aviso');

	function renderizarAmenazas() {
		if (!cuerpoAmenazas) return;
		while (cuerpoAmenazas.firstChild) {
			cuerpoAmenazas.removeChild(cuerpoAmenazas.firstChild);
		}

		listaAmenazas.forEach((am) => {
			const tr = document.createElement('tr');

			const tdFirma = document.createElement('td');
			tdFirma.textContent = am.firma || '—';

			const tdArchivo = document.createElement('td');
			tdArchivo.textContent = am.nombre || am.ruta || '—';
			tdArchivo.title = am.ruta || '';

			const tdDesc = document.createElement('td');
			tdDesc.textContent = am.descripcion || '—';

			const tdFecha = document.createElement('td');
			tdFecha.textContent = am.fecha || am.detectada || '—';

			const tdCuarentena = document.createElement('td');
			tdCuarentena.textContent = am.cuarentena ? 'Sí' : 'No';

			tr.appendChild(tdFirma);
			tr.appendChild(tdArchivo);
			tr.appendChild(tdDesc);
			tr.appendChild(tdFecha);
			tr.appendChild(tdCuarentena);

			cuerpoAmenazas.appendChild(tr);
		});
	}

	function renderizarCuarentena() {
		if (!cuerpoCuarentena) return;
		while (cuerpoCuarentena.firstChild) {
			cuerpoCuarentena.removeChild(cuerpoCuarentena.firstChild);
		}

		listaCuarentena.forEach((c) => {
			const tr = document.createElement('tr');

			const tdArchivo = document.createElement('td');
			tdArchivo.textContent = c.nombre || c.ruta || '—';
			tdArchivo.title = c.ruta || '';

			const tdTipo = document.createElement('td');
			tdTipo.textContent = c.tipo || '—';

			const tdMotivo = document.createElement('td');
			tdMotivo.textContent = c.motivo || '—';

			const tdUsuario = document.createElement('td');
			tdUsuario.textContent = c.usuario || '—';

			const tdFecha = document.createElement('td');
			tdFecha.textContent = c.fecha || '—';

			const tdPerm = document.createElement('td');
			tdPerm.textContent = c.permisos_anteriores || '—';

			tr.appendChild(tdArchivo);
			tr.appendChild(tdTipo);
			tr.appendChild(tdMotivo);
			tr.appendChild(tdUsuario);
			tr.appendChild(tdFecha);
			tr.appendChild(tdPerm);

			cuerpoCuarentena.appendChild(tr);
		});
	}

	function cargarAmenazas() {
		api.get('/api/amenazas')
			.then((r) => r.json())
			.then((d) => {
				if (d && Array.isArray(d.amenazas)) {
					listaAmenazas = d.amenazas.slice(0, 100);
					renderizarAmenazas();
				}
			})
			.catch(() => {});
	}

	function agregarAmenaza(am) {
		if (!am) return;
		const idx = listaAmenazas.findIndex((item) => item.id === am.id);
		if (idx >= 0) {
			listaAmenazas[idx] = am;
		} else {
			listaAmenazas.unshift(am);
		}
		if (listaAmenazas.length > 100) {
			listaAmenazas = listaAmenazas.slice(0, 100);
		}
		renderizarAmenazas();
	}

	function cargarCuarentena() {
		if (!window.auth || !window.auth.esAdmin()) return;
		api.get('/api/cuarentena')
			.then((r) => r.json())
			.then((d) => {
				if (d && Array.isArray(d.cuarentena)) {
					listaCuarentena = d.cuarentena.slice(0, 256);
					renderizarCuarentena();
				}
			})
			.catch(() => {});
	}

	function cargarBlacklist() {
		if (!window.auth || !window.auth.esAdmin()) return;
		api.get('/api/blacklist')
			.then((r) => r.json())
			.then((d) => {
				if (!d) return;
				if (nombreBlacklist)
					nombreBlacklist.textContent = d.nombre || '—';
				if (versionBlacklist)
					versionBlacklist.textContent = d.version || '—';
				if (fechaBlacklist)
					fechaBlacklist.textContent = d.actualizado || d.fecha || d.cargada || '—';
				if (cantidadBlacklist) {
					if (typeof d.firmas === 'number')
						cantidadBlacklist.textContent = d.firmas;
					else if (typeof d.cantidad === 'number')
						cantidadBlacklist.textContent = d.cantidad;
					else if (Array.isArray(d.firmas))
						cantidadBlacklist.textContent = d.firmas.length;
					else
						cantidadBlacklist.textContent = '—';
				}
				if (errorBlacklist) {
					if (d.error) {
						errorBlacklist.textContent = d.error;
						errorBlacklist.hidden = false;
					} else {
						errorBlacklist.textContent = '';
						errorBlacklist.hidden = true;
					}
				}
			})
			.catch(() => {});
	}

	function conectarControles() {
		if (!formBlacklist) return;
		formBlacklist.addEventListener('submit', async (ev) => {
			ev.preventDefault();
			if (!inputBlacklist || !inputBlacklist.files || inputBlacklist.files.length === 0)
				return;
			const archivo = inputBlacklist.files[0];
			if (archivo.size > 1048576) {
				if (avisoBlacklist) {
					avisoBlacklist.textContent = 'El archivo supera el tamaño máximo permitido de 1 MiB';
					avisoBlacklist.className = 'blacklist-aviso error';
				}
				return;
			}

			let texto;
			try {
				texto = await archivo.text();
			} catch (e) {
				if (avisoBlacklist) {
					avisoBlacklist.textContent = 'No se pudo leer el archivo local (verifique permisos)';
					avisoBlacklist.className = 'blacklist-aviso error';
				}
				return;
			}

			try {
				const res = await api.post('/api/blacklist', texto);
				const d = await res.json().catch(() => ({}));
				if (!res.ok) {
					if (avisoBlacklist) {
						avisoBlacklist.textContent = d.error || 'No se pudo actualizar la blacklist';
						avisoBlacklist.className = 'blacklist-aviso error';
					}
					return;
				}
				if (avisoBlacklist) {
					avisoBlacklist.textContent = 'Blacklist actualizada: ' + (d.firmas || 0) + ' firmas';
					avisoBlacklist.className = 'blacklist-aviso exito';
				}
				inputBlacklist.value = '';
				cargarBlacklist();
			} catch (err) {
				if (avisoBlacklist) {
					avisoBlacklist.textContent = 'Error de conexión con el daemon';
					avisoBlacklist.className = 'blacklist-aviso error';
				}
			}
		});
	}

	function alAlerta(a) {
		if (a && a.motivo === 'Archivo puesto en cuarentena' && window.auth && window.auth.esAdmin()) {
			cargarCuarentena();
		}
	}

	return {
		cargarAmenazas,
		agregarAmenaza,
		cargarCuarentena,
		cargarBlacklist,
		conectarControles,
		alAlerta
	};
})();
