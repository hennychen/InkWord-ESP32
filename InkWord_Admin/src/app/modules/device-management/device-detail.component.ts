import { Component, inject, signal } from '@angular/core';
import { CommonModule } from '@angular/common';
import { MAT_DIALOG_DATA, MatDialogRef, MatDialogModule } from '@angular/material/dialog';
import { MatButtonModule } from '@angular/material/button';
import { MatIconModule } from '@angular/material/icon';
import { MatCardModule } from '@angular/material/card';
import { MatSnackBar } from '@angular/material/snack-bar';
import { DeviceApiService } from '../../core/api/device-api.service';
import { Device, DeviceCommandDto } from '../../core/models/models';

/**
 * 设备远程命令面板 (A-10)：
 * 展示设备详情 + 按钮组：强制同步、强制全刷、推送 OTA。
 * 发送指令后显示"指令已下发"反馈。
 */
@Component({
  selector: 'app-device-detail',
  standalone: true,
  imports: [
    CommonModule,
    MatDialogModule,
    MatButtonModule,
    MatIconModule,
    MatCardModule,
  ],
  templateUrl: './device-detail.component.html',
  styleUrl: './device-detail.component.scss',
})
export class DeviceDetailComponent {
  private readonly deviceApi = inject(DeviceApiService);
  private readonly dialogRef = inject(MatDialogRef<DeviceDetailComponent>);
  private readonly snackBar = inject(MatSnackBar);
  readonly device = inject<Device>(MAT_DIALOG_DATA);

  readonly sending = signal(false);

  /** 模拟电量曲线数据（实际可从后端获取历史记录） */
  readonly batteryHistory = [
    { time: '08:00', battery: 95 },
    { time: '12:00', battery: 88 },
    { time: '16:00', battery: 82 },
    { time: '20:00', battery: 78 },
  ];

  get isOnline(): boolean {
    if (!this.device.lastHeartbeat) return false;
    const diff = Date.now() - new Date(this.device.lastHeartbeat).getTime();
    return diff < 5 * 60 * 1000;
  }

  /** 下发指令 */
  sendCommand(action: DeviceCommandDto['action']): void {
    this.sending.set(true);
    const dto: DeviceCommandDto = {
      deviceId: this.device.id,
      action,
    };

    this.deviceApi.sendCommand(dto).subscribe({
      next: () => {
        this.sending.set(false);
        const labels: Record<string, string> = {
          force_sync: '强制同步',
          force_refresh: '强制全刷',
          push_ota: '推送 OTA',
        };
        this.snackBar.open(`${labels[action]} 指令已下发`, '关闭', { duration: 3000 });
      },
      error: () => this.sending.set(false),
    });
  }

  close(): void {
    this.dialogRef.close();
  }
}
