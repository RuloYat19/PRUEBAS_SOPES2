'use strict';

const metricas = (() => {
	const KB_POR_GB = 1048576;
	const MAX_MUESTRAS = 150;

	const $ = (id) => document.getElementById(id);
	const gb = (kb) => (kb / KB_POR_GB).toFixed(2) + ' GB';
	const pct = (val, total) => (total ? Math.round((val / total) * 100) : 0) + ' %';

	const muestras = [];

	function parsearTiempo(ts) {
		const parsed = new Date(ts.replace(' ', 'T')).getTime();
		return isNaN(parsed) ? Date.now() : parsed;
	}

	function repintar() {
		if (!muestras.length)
			return;

		const ult = muestras[muestras.length - 1];
		const tiempos = muestras.map((m) => parsearTiempo(m.timestamp));

		const paginasCont = $('grafico-paginas');
		const paginasLey = $('leyenda-paginas');
		if (paginasCont && paginasLey) {
			Graficos.lineas(paginasCont, tiempos, [
				{ etiqueta: 'Páginas activas', corta: 'Activas', color: 'var(--serie-activas)', valores: muestras.map((m) => m.activas / KB_POR_GB) },
				{ etiqueta: 'Páginas inactivas', corta: 'Inactivas', color: 'var(--serie-inactivas)', valores: muestras.map((m) => m.inactivas / KB_POR_GB) }
			], (v) => v.toFixed(2) + ' GB', 'Páginas activas e inactivas en el tiempo');
			Graficos.leyenda(paginasLey, [
				{ color: 'var(--serie-activas)', etiqueta: 'Páginas activas', valor: gb(ult.activas) },
				{ color: 'var(--serie-inactivas)', etiqueta: 'Páginas inactivas', valor: gb(ult.inactivas) }
			]);
		}

		const evolCont = $('grafico-lineas');
		const evolLey = $('leyenda-lineas');
		if (evolCont && evolLey) {
			Graficos.lineas(evolCont, tiempos, [
				{ etiqueta: 'Memoria física usada', corta: 'Física', color: 'var(--serie-usada)', valores: muestras.map((m) => m.usada / KB_POR_GB) },
				{ etiqueta: 'Swap usada', corta: 'Swap', color: 'var(--serie-swap)', valores: muestras.map((m) => m.swap_usada / KB_POR_GB) }
			], (v) => v.toFixed(2) + ' GB', 'Memoria física usada y swap usada en el tiempo');
			Graficos.leyenda(evolLey, [
				{ color: 'var(--serie-usada)', etiqueta: 'Memoria física usada', valor: gb(ult.usada) },
				{ color: 'var(--serie-swap)', etiqueta: 'Swap usada', valor: gb(ult.swap_usada) }
			]);
		}

		const donaCont = $('grafico-dona');
		const donaLey = $('leyenda-dona');
		if (donaCont && donaLey) {
			const total = ult.mem_total;
			const segmentos = [
				{ etiqueta: 'Utilizada', detalle: 'Memoria usada', valor: ult.usada, color: 'var(--serie-usada)' },
				{ etiqueta: 'Caché', detalle: 'Buffers + Cached', valor: ult.cache, color: 'var(--serie-cache)' },
				{ etiqueta: 'Libre', detalle: 'Memoria libre', valor: ult.libre, color: 'var(--serie-libre)' }
			];
			segmentos.forEach((s) => {
				s.texto = `${gb(s.valor)} (${pct(s.valor, total)})`;
			});
			const memTotalEl = $('mem-total');
			if (memTotalEl)
				memTotalEl.textContent = gb(total);
			Graficos.dona(donaCont, segmentos, { valor: pct(ult.usada, total), etiqueta: 'en uso' });
			Graficos.leyenda(donaLey, segmentos.map((s) => ({
				color: s.color, etiqueta: s.etiqueta, valor: gb(s.valor), detalle: pct(s.valor, total)
			})));
		}

		const pgfaultCont = $('grafico-pgfault');
		const pgfaultLey = $('leyenda-pgfault');
		if (pgfaultCont && pgfaultLey) {
			Graficos.lineas(pgfaultCont, tiempos, [
				{ etiqueta: 'Fallos menores (pgfault)', corta: 'Menores', color: 'var(--serie-pgfault)', valores: muestras.map((m) => Number(m.pgfault_delta)) }
			], (v) => Math.round(v).toLocaleString('es-GT'), 'Fallos menores de página por intervalo');
			Graficos.leyenda(pgfaultLey, [
				{ color: 'var(--serie-pgfault)', etiqueta: 'Fallos menores (pgfault)', valor: Math.round(ult.pgfault_delta).toLocaleString('es-GT') }
			]);
		}

		const pgmajCont = $('grafico-pgmajfault');
		const pgmajLey = $('leyenda-pgmajfault');
		if (pgmajCont && pgmajLey) {
			Graficos.lineas(pgmajCont, tiempos, [
				{ etiqueta: 'Fallos mayores (pgmajfault)', corta: 'Mayores', color: 'var(--serie-pgmajfault)', valores: muestras.map((m) => Number(m.pgmajfault_delta)) }
			], (v) => Math.round(v).toLocaleString('es-GT'), 'Fallos mayores de página por intervalo');
			Graficos.leyenda(pgmajLey, [
				{ color: 'var(--serie-pgmajfault)', etiqueta: 'Fallos mayores (pgmajfault)', valor: Math.round(ult.pgmajfault_delta).toLocaleString('es-GT') }
			]);
		}
	}

	function agregar(m) {
		if (!m || typeof m.id !== 'number')
			return;
		if (muestras.some((item) => item.id === m.id))
			return;

		muestras.push(m);
		muestras.sort((a, b) => a.id - b.id);
		while (muestras.length > MAX_MUESTRAS)
			muestras.shift();

		repintar();
	}

	async function cargarHistorial() {
		try {
			const respuesta = await api.get('/api/metricas');
			if (!respuesta.ok)
				return;
			const datos = await respuesta.json();
			if (datos && Array.isArray(datos.muestras)) {
				for (const m of datos.muestras)
					agregar(m);
			}
			if (datos && typeof datos.activo === 'boolean')
				aplicarEstado(datos.activo);
		} catch (e) {
		}
	}

	function aplicarEstado(activo) {
		const interruptor = $('interruptor-metricas');
		if (interruptor)
			interruptor.checked = !!activo;

		const estadoTexto = $('estado-metricas');
		if (estadoTexto)
			estadoTexto.textContent = activo ? 'Métricas activas' : 'Métricas desactivadas';

		const avisosMetricas = document.querySelectorAll(
			'#panel-paginas .aviso-desactivado, #panel-evolucion .aviso-desactivado, #panel-desglose .aviso-desactivado, #panel-fallos .aviso-desactivado'
		);
		avisosMetricas.forEach((el) => {
			el.hidden = !!activo;
		});
	}

	function conectarInterruptor() {
		const interruptor = $('interruptor-metricas');
		if (!interruptor || interruptor.dataset.conectado)
			return;

		interruptor.dataset.conectado = 'true';
		interruptor.addEventListener('change', async () => {
			const deseado = interruptor.checked;
			try {
				const respuesta = await api.post('/api/monitoreo/metricas', { activo: deseado });
				if (!respuesta.ok)
					throw new Error(respuesta.status);
				const datos = await respuesta.json();
				if (datos && typeof datos.metricas === 'boolean')
					aplicarEstado(datos.metricas);
			} catch (e) {
				interruptor.checked = !deseado;
			}
		});
	}

	return {
		cargarHistorial,
		agregar,
		aplicarEstado,
		conectarInterruptor
	};
})();
