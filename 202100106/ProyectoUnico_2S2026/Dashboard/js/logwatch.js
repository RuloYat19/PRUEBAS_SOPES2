'use strict';

const logwatch = (() => {
	let directorioBase = '';

	const spanBase = document.getElementById('logwatch-base');
	const formLogwatch = document.getElementById('form-logwatch');
	const inputPalabra = document.getElementById('logwatch-palabra');
	const inputCentral = document.getElementById('logwatch-central');
	const inputRutas = document.getElementById('logwatch-rutas');
	const aviso = document.getElementById('logwatch-aviso');
	const cuerpo = document.getElementById('logwatch-cuerpo');

	function mostrarAviso(texto, esError) {
		if (!aviso) return;
		aviso.textContent = texto || '';
		aviso.className = 'logwatch-aviso' + (esError ? ' error' : ' exito');
	}

	function limpiarAviso() {
		if (!aviso) return;
		aviso.textContent = '';
		aviso.className = 'logwatch-aviso';
	}

	function renderizarTabla(items) {
		if (!cuerpo) return;
		while (cuerpo.firstChild) {
			cuerpo.removeChild(cuerpo.firstChild);
		}
		if (!Array.isArray(items)) return;

		items.forEach((item) => {
			const tr = document.createElement('tr');

			const tdId = document.createElement('td');
			tdId.textContent = item.id;

			const tdPalabra = document.createElement('td');
			tdPalabra.textContent = item.palabra || '—';

			const tdCentral = document.createElement('td');
			tdCentral.textContent = item.central || '—';

			const tdRutas = document.createElement('td');
			if (Array.isArray(item.rutas)) {
				tdRutas.textContent = item.rutas.join(', ');
			} else {
				tdRutas.textContent = '—';
			}

			const tdOrigen = document.createElement('td');
			tdOrigen.textContent = item.origen || '—';

			const tdUsuario = document.createElement('td');
			tdUsuario.textContent = item.usuario || '—';

			const tdFecha = document.createElement('td');
			tdFecha.textContent = item.creado || '—';

			const tdAccion = document.createElement('td');
			const btn = document.createElement('button');
			btn.type = 'button';
			btn.className = 'boton-finalizar';
			btn.textContent = 'Finalizar';
			btn.addEventListener('click', () => finalizarLogwatch(item.id, item.palabra));
			tdAccion.appendChild(btn);

			tr.appendChild(tdId);
			tr.appendChild(tdPalabra);
			tr.appendChild(tdCentral);
			tr.appendChild(tdRutas);
			tr.appendChild(tdOrigen);
			tr.appendChild(tdUsuario);
			tr.appendChild(tdFecha);
			tr.appendChild(tdAccion);

			cuerpo.appendChild(tr);
		});
	}

	function cargarLogwatch() {
		if (!window.auth || !window.auth.esAdmin()) return;
		api.get('/api/logwatch')
			.then((r) => r.json())
			.then((d) => {
				if (!d) return;
				if (typeof d.base === 'string') {
					directorioBase = d.base;
					if (spanBase) {
						spanBase.textContent = d.base;
					}
					if (inputCentral && !inputCentral.value) {
						inputCentral.value = d.base + '/';
					}
				}
				if (Array.isArray(d.logwatch)) {
					renderizarTabla(d.logwatch);
				}
			})
			.catch(() => {});
	}

	function crearLogwatch(ev) {
		if (ev) ev.preventDefault();
		if (!inputPalabra || !inputCentral || !inputRutas) return;

		const palabra = inputPalabra.value.trim();
		const central = inputCentral.value.trim();
		const lineas = inputRutas.value
			.split('\n')
			.map((s) => s.trim())
			.filter((s) => s.length > 0);

		const cuerpoPeticion = {
			palabra: palabra,
			central: central,
			rutas: lineas
		};

		limpiarAviso();

		api.post('/api/logwatch', cuerpoPeticion)
			.then(async (r) => {
				const d = await r.json();
				if (!r.ok) {
					mostrarAviso(d.error || 'Error al crear logwatch', true);
				} else {
					mostrarAviso('Logwatch ' + d.id + ' creado', false);
					inputPalabra.value = '';
					inputRutas.value = '';
					if (inputCentral && directorioBase) {
						inputCentral.value = directorioBase + '/';
					}
					cargarLogwatch();
				}
			})
			.catch(() => {
				mostrarAviso('Error de conexión con el daemon', true);
			});
	}

	function finalizarLogwatch(id, palabra) {
		const mensaje = '¿Finalizar el logwatch ' + id + ' (' + palabra + ')?';
		if (!confirm(mensaje)) return;

		limpiarAviso();

		api.delete('/api/logwatch?id=' + encodeURIComponent(id))
			.then(async (r) => {
				const d = await r.json();
				if (!r.ok) {
					mostrarAviso(d.error || 'Error al finalizar logwatch', true);
				} else {
					mostrarAviso('Logwatch ' + id + ' finalizado', false);
					cargarLogwatch();
				}
			})
			.catch(() => {
				mostrarAviso('Error de conexión con el daemon', true);
			});
	}

	function conectarControles() {
		if (formLogwatch) {
			formLogwatch.addEventListener('submit', crearLogwatch);
		}
	}

	return {
		cargarLogwatch,
		conectarControles
	};
})();

window.logwatch = logwatch;
