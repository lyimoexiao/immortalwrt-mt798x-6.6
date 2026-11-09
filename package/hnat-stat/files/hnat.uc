'use strict';

const fs = require('fs');

return {
	hnat: {
		rates: {
			call: function() {
				let data = null;

				try {
					data = fs.readfile('/tmp/hnat_rates.json');
				} catch (e) {
					return {};
				}
				if (!data)
					return {};
				try {
					return json(data);
				} catch (e) {
					return {};
				}
			}
		}
	}
};
