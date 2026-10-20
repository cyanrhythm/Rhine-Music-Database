const fs = require('node:fs');
const path = require('node:path');

/** 发布 tag 在编译前确定；普通构建明确标记为开发版，避免缓存上次的发布身份。 */
function buildVersion(value = process.env.RHINE_VERSION) {
    const fallback = JSON.parse(fs.readFileSync(path.join(__dirname, '../package.json'), 'utf8')).version;
    if (!value) {
        return { version: fallback, release: false };
    }
    if (!/^v?(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$/.test(value)) {
        throw new Error('Release version must use vMAJOR.MINOR.PATCH, e.g. v1.0.0');
    }
    const version = value.replace(/^v/, '');
    if (version.split('.').some((segment) => Number(segment) > 65535)) {
        throw new Error('Release version components must be at most 65535 for Windows VERSIONINFO');
    }
    return { version, release: true };
}
module.exports = { buildVersion };
