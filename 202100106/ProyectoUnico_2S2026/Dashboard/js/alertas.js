'use strict';

const alertas = (() => {
	const MAX_ALERTAS = 200;
	const ids = new Set();
	const conteo = { LOW: 0, MEDIUM: 0, HIGH: 0 };

	const $ = (id) => document.getElementById(id);

	function sinMarcaDeTiempo(linea) {
		const prefijo = linea.match(/^\s*\[[^\]]*\d{1,2}:\d{2}[^\]]*\]\s*/);

		return prefijo && prefijo[0].length < linea.length ? linea.slice(prefijo[0].length) : linea;
	}

	function resaltar(texto, palabra) {
		const partes = palabra ? texto.split(palabra) : [texto];

		return partes.flatMap((parte, i) => {
			if (i === 0)
				return [parte];
			const marca = document.createElement('mark');
			marca.textContent = palabra;
			return [marca, parte];
		});
	}

	function actualizarResumen() {
		const resumen = $('alertas-resumen');
		if (resumen)
			resumen.textContent = `LOW ${conteo.LOW} · MEDIUM ${conteo.MEDIUM} · HIGH ${conteo.HIGH}`;

		const vacio = $('alertas-vacio');
		const cuerpo = $('alertas-cuerpo');
		if (vacio && cuerpo)
			vacio.classList.toggle('visible', cuerpo.rows.length === 0);
	}

	function crearFila(a, nueva) {
		const tr = document.createElement('tr');
		tr.dataset.id = a.id;
		tr.dataset.severidad = a.severidad;

		if (nueva)
			tr.className = 'nuevo';
		if (a.severidad === 'HIGH')
			tr.classList.add('sev-high');

		const celdaFecha = tr.insertCell();
		celdaFecha.textContent = a.timestamp;

		const celdaSev = tr.insertCell();
		const badge = document.createElement('span');
		const sevClase = a.severidad ? a.severidad.toLowerCase() : 'low';
		badge.className = `severidad sev-${sevClase}`;
		badge.textContent = a.severidad;
		celdaSev.appendChild(badge);

		const celdaTipo = tr.insertCell();
		celdaTipo.textContent = a.tipo;

		const celdaMotivo = tr.insertCell();
		celdaMotivo.textContent = a.motivo;

		const celdaDesc = tr.insertCell();
		celdaDesc.className = 'contenido';
		if (a.tipo === 'logs' && a.contenido && a.palabra_detectada) {
			celdaDesc.title = a.contenido;
			const partes = resaltar(sinMarcaDeTiempo(a.contenido), a.palabra_detectada);
			celdaDesc.append(...partes);
		} else {
			celdaDesc.textContent = a.descripcion;
			celdaDesc.title = a.descripcion;
		}

		return tr;
	}

	function agregar(a, nueva) {
		if (!a || ids.has(a.id))
			return;

		const cuerpo = $('alertas-cuerpo');
		if (!cuerpo)
			return;

		ids.add(a.id);
		if (conteo[a.severidad] !== undefined)
			conteo[a.severidad]++;

		const tr = crearFila(a, nueva);

		const siguiente = [...cuerpo.rows].find((fila) => Number(fila.dataset.id) < a.id);
		cuerpo.insertBefore(tr, siguiente || null);

		while (cuerpo.rows.length > MAX_ALERTAS) {
			const ultima = cuerpo.lastElementChild;
			const idBorrar = Number(ultima.dataset.id);
			const sevBorrar = ultima.dataset.severidad;
			ids.delete(idBorrar);
			if (conteo[sevBorrar] !== undefined && conteo[sevBorrar] > 0)
				conteo[sevBorrar]--;
			ultima.remove();
		}

		actualizarResumen();
	}

	function limpiar() {
		const cuerpo = $('alertas-cuerpo');
		if (cuerpo)
			cuerpo.replaceChildren();
		ids.clear();
		conteo.LOW = 0;
		conteo.MEDIUM = 0;
		conteo.HIGH = 0;
		actualizarResumen();
	}

	async function cargarHistorial() {
		try {
			const resp = await api.get('/api/alertas');
			if (!resp.ok)
				return;
			const datos = await resp.json();
			if (Array.isArray(datos.alertas)) {
				for (const a of datos.alertas)
					agregar(a, false);
			}
		} catch (e) {
		}
	}

	return {
		agregar,
		limpiar,
		cargarHistorial
	};
})();
