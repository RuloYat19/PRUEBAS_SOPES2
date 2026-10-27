'use strict';

const api = (() => {
	async function peticion(ruta, opciones) {
		const configuracion = Object.assign({ cache: 'no-store' }, opciones);
		const respuesta = await fetch(ruta, configuracion);

		if (respuesta.status === 401 && !location.pathname.endsWith('login.html')) {
			location.href = '/login.html';
		}
		return respuesta;
	}

	function get(ruta) {
		return peticion(ruta, { method: 'GET' });
	}

	function post(ruta, cuerpo) {
		const opciones = { method: 'POST' };

		if (cuerpo !== undefined) {
			opciones.headers = { 'Content-Type': 'application/json' };
			opciones.body = typeof cuerpo === 'string' ? cuerpo : JSON.stringify(cuerpo);
		}
		return peticion(ruta, opciones);
	}

	function eliminar(ruta) {
		return peticion(ruta, { method: 'DELETE' });
	}

	function sesion() {
		return get('/api/sesion');
	}

	return { get, post, del: eliminar, delete: eliminar, sesion };
})();
